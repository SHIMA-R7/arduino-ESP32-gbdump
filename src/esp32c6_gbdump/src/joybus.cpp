#include "joybus.h"

#include <string.h>
#include <stdio.h>
#include <vector>

#include "driver/rmt_tx.h"
#include "driver/rmt_rx.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

// This is a generic ESP32 driver for the N64/GameCube "Joybus" protocol
// (1-wire, half-duplex, ~250kbit/s bit-banged serial): send an arbitrary
// command byte sequence, get back an arbitrary response bit sequence. It
// is not specific to GB-cartridge-via-Transfer-Pak dumping -- the same
// transact() call is what you'd use to poll a plain N64 controller's
// button/stick state (send the single-byte 0x01 "get status" command,
// read back the 32-bit reply), talk to a Controller Pak/Rumble Pak, etc.
// Anyone porting brownan's Gamecube-N64-Controller or fl4shk's
// arduinogbdump (both AVR-only, bit-banged with cycle-counted assembly)
// to an ESP32 can reuse this file as-is.
//
// Both directions apply the *same decode rule* the AVR original uses:
// after the line goes low, the bit is a '1' if the line is already back
// high ~2us later (i.e. it was a short ~1us low pulse) and a '0' if it's
// still low (i.e. a long ~3us low pulse). The AVR code implements that by
// literally sampling the pin once at the 2us mark; we get the equivalent
// answer by having the RMT peripheral measure the low phase's duration in
// hardware and thresholding it -- same rule, hardware-timed instead of
// software-timed. A pure software busy-loop (interrupts disabled,
// microsecond hardware timer) was also tried and is *less* reliable here:
// the AVR original gets its precision from hand-counted NOPs in inline
// assembly, which has no real equivalent under the ESP32 Arduino/IDF
// framework -- even with interrupts off, C function-call overhead
// (gpio_get_level(), the timer read) is enough to blow the 1us budget.
// RMT is the standard way real ESP32 Joybus/WS2812/IR projects get
// genuine cycle-accurate timing, so that's what both directions use here.

namespace joybus {

namespace {

static const char *TAG = "joybus";

// 1 tick = 1 microsecond.
static const uint32_t RESOLUTION_HZ = 1000000;

gpio_num_t s_tx_pin = GPIO_NUM_NC;
gpio_num_t s_rx_pin = GPIO_NUM_NC;
const char *s_last_status = "not called yet";

rmt_channel_handle_t s_rx_chan = nullptr;
QueueHandle_t s_rx_queue = nullptr;

// Joybus bit timings -- identical convention to what brownan's
// Gamecube-N64-Controller / fl4shk's arduinogbdump bit-bang on AVR:
//   '0' bit: low for 3us, then high for 1us
//   '1' bit: low for 1us, then high for 3us
//   stop bit: low for 1us, then high (line released, frame over) -- we
//             reuse the '1' bit shape for this, the extra high time
//             doesn't matter since nothing follows.
rmt_symbol_word_t bit_symbol(bool bit_is_one)
{
    rmt_symbol_word_t s;
    s.level0 = 0;
    s.duration0 = bit_is_one ? 1 : 3;
    s.level1 = 1;
    s.duration1 = bit_is_one ? 3 : 1;
    return s;
}

bool IRAM_ATTR on_rx_done(rmt_channel_handle_t, const rmt_rx_done_event_data_t *edata, void *user_ctx)
{
    QueueHandle_t q = static_cast<QueueHandle_t>(user_ctx);
    BaseType_t hp_task_woken = pdFALSE;
    xQueueSendFromISR(q, edata, &hp_task_woken);
    return hp_task_woken == pdTRUE;
}

// Sends `cmd`/`cmd_len` (MSB first per byte) + a stop bit via a transient
// RMT TX channel. Blocks until fully transmitted.
//
// On success, leaves the TX channel *enabled* (not yet disabled/deleted)
// and returns its handle via `out_tx_chan` -- disabling/deleting it
// generates a small electrical transient on the shared GPIO, so the
// caller should defer that until after it's done reading any response
// (see transact() below), not do it here.
//
// IMPORTANT: `eot_level` must be set to 1 (released/high) -- otherwise,
// once left enabled after the transmission finishes, the channel defaults
// to holding the line actively LOW, which looks exactly like a real
// (bogus) low pulse to anything reading the pin afterwards.
bool send_via_rmt(const uint8_t *cmd, size_t cmd_len, rmt_channel_handle_t *out_tx_chan)
{
    *out_tx_chan = nullptr;
    std::vector<rmt_symbol_word_t> tx_symbols;
    tx_symbols.reserve(cmd_len * 8 + 1);
    for (size_t i = 0; i < cmd_len; ++i) {
        uint8_t byte = cmd[i];
        for (int bit = 7; bit >= 0; --bit) {
            tx_symbols.push_back(bit_symbol((byte >> bit) & 0x1));
        }
    }
    tx_symbols.push_back(bit_symbol(true)); // stop bit

    rmt_tx_channel_config_t tx_cfg = {};
    tx_cfg.gpio_num = s_tx_pin;
    tx_cfg.clk_src = RMT_CLK_SRC_DEFAULT;
    tx_cfg.resolution_hz = RESOLUTION_HZ;
    tx_cfg.mem_block_symbols = 64;
    tx_cfg.trans_queue_depth = 1;
    tx_cfg.flags.io_od_mode = true; // open-drain: only actively drives low

    rmt_channel_handle_t tx_chan = nullptr;
    bool ok = true;
    if (rmt_new_tx_channel(&tx_cfg, &tx_chan) != ESP_OK) {
        ESP_LOGE(TAG, "rmt_new_tx_channel failed");
        s_last_status = "rmt_new_tx_channel failed";
        return false;
    }
    if (rmt_enable(tx_chan) != ESP_OK) {
        ESP_LOGE(TAG, "rmt_enable(tx) failed");
        s_last_status = "rmt_enable(tx) failed";
        rmt_del_channel(tx_chan);
        return false;
    }

    rmt_encoder_handle_t copy_encoder = nullptr;
    rmt_copy_encoder_config_t copy_cfg = {};
    if (rmt_new_copy_encoder(&copy_cfg, &copy_encoder) != ESP_OK) {
        ESP_LOGE(TAG, "rmt_new_copy_encoder failed");
        s_last_status = "rmt_new_copy_encoder failed";
        ok = false;
    }

    if (ok) {
        rmt_transmit_config_t tx_transmit_cfg = {};
        tx_transmit_cfg.loop_count = 0;
        tx_transmit_cfg.flags.eot_level = 1; // hold released/high, not low
        if (rmt_transmit(tx_chan, copy_encoder, tx_symbols.data(),
                          tx_symbols.size() * sizeof(rmt_symbol_word_t),
                          &tx_transmit_cfg) != ESP_OK) {
            ESP_LOGE(TAG, "rmt_transmit failed");
            s_last_status = "rmt_transmit failed";
            ok = false;
        } else if (rmt_tx_wait_all_done(tx_chan, 100) != ESP_OK) {
            ESP_LOGE(TAG, "rmt_tx_wait_all_done timed out");
            s_last_status = "rmt_tx_wait_all_done timed out";
            ok = false;
        }
    }

    if (copy_encoder) {
        rmt_del_encoder(copy_encoder);
    }
    if (!ok) {
        rmt_disable(tx_chan);
        rmt_del_channel(tx_chan);
        return false;
    }
    *out_tx_chan = tx_chan; // caller tears this down later
    return true;
}

} // namespace

void init(gpio_num_t tx_pin, gpio_num_t rx_pin)
{
    s_tx_pin = tx_pin;
    s_rx_pin = rx_pin;

    gpio_reset_pin(tx_pin);
    gpio_set_direction(tx_pin, GPIO_MODE_INPUT);
    // Internal pull-up as a safety net; an external ~1k pull-up to 3.3V
    // (as in the original schematic) is required for signal integrity --
    // the internal pull-up alone is far too weak (tens of kOhm) to give
    // clean, fast-enough edges for reliable 1us-resolution timing.
    gpio_set_pull_mode(tx_pin, GPIO_PULLUP_ONLY);

    if (rx_pin != tx_pin) {
        gpio_reset_pin(rx_pin);
        gpio_set_direction(rx_pin, GPIO_MODE_INPUT);
        gpio_set_pull_mode(rx_pin, GPIO_PULLUP_ONLY);
    }

    // Create the RX channel once, on its own dedicated pin, and reuse it
    // for every transaction. Using a separate pin from TX (wired together
    // externally with a jumper to the same physical bus) means this
    // channel never shares a GPIO with a TX channel, sidestepping some
    // RMT TX/RX channel interaction on shared pins observed while
    // bringing this up (see the git history / conversation for details --
    // short version: on this chip/driver, having a TX channel and this RX
    // channel both live on the same GPIO produced data that didn't look
    // trustworthy, and tearing the TX channel down could even knock the
    // RX channel out of its enabled state).
    rmt_rx_channel_config_t rx_cfg = {};
    rx_cfg.gpio_num = s_rx_pin;
    rx_cfg.clk_src = RMT_CLK_SRC_DEFAULT;
    rx_cfg.resolution_hz = RESOLUTION_HZ;
    rx_cfg.mem_block_symbols = 64;
    rx_cfg.flags.io_loop_back = false;

    if (rmt_new_rx_channel(&rx_cfg, &s_rx_chan) != ESP_OK) {
        ESP_LOGE(TAG, "rmt_new_rx_channel failed");
        s_last_status = "rmt_new_rx_channel failed (init)";
        return;
    }

    s_rx_queue = xQueueCreate(1, sizeof(rmt_rx_done_event_data_t));

    rmt_rx_event_callbacks_t cbs = {};
    cbs.on_recv_done = on_rx_done;
    rmt_rx_register_event_callbacks(s_rx_chan, &cbs, s_rx_queue);

    if (rmt_enable(s_rx_chan) != ESP_OK) {
        ESP_LOGE(TAG, "rmt_enable(rx) failed");
        s_last_status = "rmt_enable(rx) failed (init)";
        return;
    }
    s_last_status = "init ok";
}

size_t transact(const uint8_t *cmd, size_t cmd_len, uint8_t *out_bytes, size_t out_bits,
                 uint32_t timeout_us)
{
    if (s_tx_pin == GPIO_NUM_NC || s_rx_chan == nullptr || cmd_len == 0) {
        s_last_status = "not initialized";
        return 0;
    }
    memset(out_bytes, 0, (out_bits + 7) / 8);

    // RX is armed *before* TX sends anything (arming after was found to
    // risk missing the very start of a fast-arriving response), so the
    // capture also includes our own outgoing command + stop bit. Those
    // leading bits are skipped below -- callers only see the actual
    // response.
    size_t own_bits = cmd_len * 8 + 1;
    size_t max_symbols = own_bits + out_bits + 16; // + margin for glitches
    std::vector<rmt_symbol_word_t> rx_symbols(max_symbols);

    rmt_receive_config_t rx_receive_cfg = {};
    rx_receive_cfg.signal_range_min_ns = 200;
    rx_receive_cfg.signal_range_max_ns = 12000;
    bool rx_armed = rmt_receive(s_rx_chan, rx_symbols.data(), max_symbols * sizeof(rmt_symbol_word_t),
                                 &rx_receive_cfg) == ESP_OK;
    if (!rx_armed) {
        ESP_LOGE(TAG, "rmt_receive (arm) failed");
        s_last_status = "rmt_receive() arm failed";
    }

    rmt_channel_handle_t tx_chan = nullptr;
    if (!send_via_rmt(cmd, cmd_len, &tx_chan)) {
        return 0;
    }

    // Wait for the response while the TX channel is still around -- tear
    // it down only after we're done reading, so its teardown transient
    // can't corrupt the capture.
    size_t decoded_bits = 0;
    if (rx_armed) {
        rmt_rx_done_event_data_t rx_data;
        if (xQueueReceive(s_rx_queue, &rx_data, pdMS_TO_TICKS(timeout_us / 1000 + 5)) == pdTRUE) {
            static char status_buf[48];
            snprintf(status_buf, sizeof(status_buf), "ok, %u raw symbols", (unsigned)rx_data.num_symbols);
            s_last_status = status_buf;
            for (size_t i = own_bits; i < rx_data.num_symbols && decoded_bits < out_bits; ++i) {
                const rmt_symbol_word_t &sym = rx_data.received_symbols[i];
                // RMT RX just records alternating segments starting from
                // whatever level the line was at when armed -- it does
                // NOT guarantee duration0 is the low phase (unlike our TX
                // symbols, where we always put the low phase first).
                // Figure out which duration is the low (active) phase,
                // then apply the same "short low = 1, long low = 0" rule
                // the AVR original uses (there: sampled at 2us; here:
                // thresholded at the same 2us mark against the measured
                // duration -- equivalent decision, hardware-timed).
                uint16_t low_dur = (sym.level0 == 0) ? sym.duration0 : sym.duration1;
                bool bit_is_one = low_dur <= 2;
                if (bit_is_one) {
                    out_bytes[decoded_bits / 8] |= (0x80 >> (decoded_bits % 8));
                }
                ++decoded_bits;
            }
        } else {
            ESP_LOGW(TAG, "receive timed out waiting for response");
            s_last_status = "xQueueReceive timed out (no edges captured)";
            // The RMT hardware reception we armed above is likely still
            // pending (it only auto-completes via its own idle-timeout
            // *after* at least one edge arrives; if none ever did, it
            // just sits there waiting forever). Left alone, this leaves
            // the channel "busy" and every future rmt_receive() call
            // fails with ESP_ERR_INVALID_STATE ("channel not in enable
            // state"). Reset by cycling disable/enable so the next
            // transact() can arm cleanly.
            rmt_disable(s_rx_chan);
            rmt_enable(s_rx_chan);
        }
    }

    rmt_disable(tx_chan);
    rmt_del_channel(tx_chan);

    return decoded_bits;
}

const char *last_status()
{
    return s_last_status;
}

int sample_idle_high_count(int samples)
{
    if (s_rx_pin == GPIO_NUM_NC) {
        return -1;
    }
    int high_count = 0;
    for (int i = 0; i < samples; ++i) {
        if (gpio_get_level(s_rx_pin)) {
            ++high_count;
        }
    }
    return high_count;
}

} // namespace joybus
