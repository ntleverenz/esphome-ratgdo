/************************************
 * Rage
 * Against
 * The
 * Garage
 * Door
 * Opener
 *
 * Copyright (C) 2022  Paul Wieland
 *
 * GNU GENERAL PUBLIC LICENSE
 ************************************/

#include "ratgdo.h"
#include "ratgdo_state.h"

#include "esphome/core/log.h"

#define ESP_LOG1 ESP_LOGV
#define ESP_LOG2 ESP_LOGV

namespace esphome {
namespace ratgdo {

    static const char* const TAG = "ratgdo";
    static const int SYNC_DELAY = 1000;
    //
    // MAX_CODES_WITHOUT_FLASH_WRITE is a bit of a guess
    // since we write the flash at most every every 5s
    //
    // We want the rolling counter to be high enough that the
    // GDO will accept the command after an unexpected reboot
    // that did not save the counter to flash in time which
    // results in the rolling counter being behind what the GDO
    // expects.
    //
    static const uint8_t MAX_CODES_WITHOUT_FLASH_WRITE = 10;

    void RATGDOComponent::setup()
    {
        this->output_gdo_pin_->setup();
        this->output_gdo_pin_->pin_mode(gpio::FLAG_OUTPUT);

        this->input_gdo_pin_->setup();
        this->input_gdo_pin_->pin_mode(gpio::FLAG_INPUT | gpio::FLAG_PULLUP);
        this->input_obst_pin_->pin_mode(gpio::FLAG_INPUT);
        
        this->input_obst_pin_->attach_interrupt(RATGDOStore::isr_obstruction, &this->isr_store_, gpio::INTERRUPT_ANY_EDGE);

        this->sw_serial_.begin(9600, SWSERIAL_8N1, this->input_gdo_pin_->get_pin(), this->output_gdo_pin_->get_pin(), true);

        ESP_LOGV(TAG, "Syncing rolling code counter after reboot...");

        // many things happening at startup, use some delay for sync
        set_timeout(SYNC_DELAY, [=] { this->sync(); });
    }

    void RATGDOComponent::loop()
    {
        if (this->transmit_pending_) {
            if (!this->transmit_packet()) {
                return;
            }
        }
        if (!this->obstruction_from_status_) {
            this->obstruction_loop();
        }
        this->gdo_state_loop();
    }

    void RATGDOComponent::dump_config()
    {
        ESP_LOGCONFIG(TAG, "Setting up RATGDO...");
        LOG_PIN("  Output GDO Pin: ", this->output_gdo_pin_);
        LOG_PIN("  Input GDO Pin: ", this->input_gdo_pin_);
        if (this->obstruction_from_status_) {
            ESP_LOGCONFIG(TAG, "  Input Obstruction Pin: not used, will detect from GDO status");
        } else {
            LOG_PIN("  Input Obstruction Pin: ", this->input_obst_pin_);
        }
        ESP_LOGCONFIG(TAG, "  Rolling Code Counter: %d", *this->rolling_code_counter);
        ESP_LOGCONFIG(TAG, "  Remote ID: %d", this->remote_id_);
    }

    uint16_t RATGDOComponent::decode_packet(const WirePacket& packet)
    {
        uint32_t rolling = 0;
        uint64_t fixed = 0;
        uint32_t data = 0;

        decode_wireline(packet, &rolling, &fixed, &data);

        uint16_t cmd = ((fixed >> 24) & 0xf00) | (data & 0xff);
        data &= ~0xf000; // clear parity nibble

        if ((fixed & 0xfffffff) == this->remote_id_) { // my commands
            ESP_LOG1(TAG, "[%ld] received mine: rolling=%07" PRIx32 " fixed=%010" PRIx64 " data=%08" PRIx32, millis(), rolling, fixed, data);
            return static_cast<uint16_t>(Command::UNKNOWN);
        } else {
            ESP_LOG1(TAG, "[%ld] received rolling=%07" PRIx32 " fixed=%010" PRIx64 " data=%08" PRIx32, millis(), rolling, fixed, data);
        }

        Command cmd_enum = to_Command(cmd, Command::UNKNOWN);
        uint8_t nibble = (data >> 8) & 0xff;
        uint8_t byte1 = (data >> 16) & 0xff;
        uint8_t byte2 = (data >> 24) & 0xff;

        ESP_LOG1(TAG, "cmd=%03x (%s) byte2=%02x byte1=%02x nibble=%01x", cmd, Command_to_string(cmd_enum), byte2, byte1, nibble);

        if (cmd == Command::STATUS) {

            auto door_state = to_DoorState(nibble, DoorState::UNKNOWN);
            auto prev_door_state = *this->door_state;
            query_status_flags_ |= QSF_STATUS;

            // opening duration calibration
            if (*this->opening_duration == 0) {
                if (door_state == DoorState::OPENING && prev_door_state == DoorState::CLOSED) {
                    this->start_opening = millis();
                }
                if (door_state == DoorState::OPEN && prev_door_state == DoorState::OPENING && this->start_opening > 0) {
                    auto duration = (millis() - this->start_opening) / 1000;
                    this->set_opening_duration(round(duration * 10) / 10);
                }
                if (door_state == DoorState::STOPPED) {
                    this->start_opening = -1;
                }
            }
            // closing duration calibration
            if (*this->closing_duration == 0) {
                if (door_state == DoorState::CLOSING && prev_door_state == DoorState::OPEN) {
                    this->start_closing = millis();
                }
                if (door_state == DoorState::CLOSED && prev_door_state == DoorState::CLOSING && this->start_closing > 0) {
                    auto duration = (millis() - this->start_closing) / 1000;
                    this->set_closing_duration(round(duration * 10) / 10);
                }
                if (door_state == DoorState::STOPPED) {
                    this->start_closing = -1;
                }
            }

            if (door_state == DoorState::OPENING) {
                // door started opening
                if (prev_door_state == DoorState::CLOSING) {
                    this->door_position_update();
                    this->cancel_position_sync_callbacks();
                    this->door_move_delta = DOOR_DELTA_UNKNOWN;
                }
                this->door_start_moving = millis();
                this->door_start_position = *this->door_position;
                if (this->door_move_delta == DOOR_DELTA_UNKNOWN) {
                    this->door_move_delta = 1.0 - this->door_start_position;
                }
                this->schedule_door_position_sync();

                // this would only get called if no status message is received after door stops moving
                // request a status message in that case
                set_timeout("door_status_update", (*this->opening_duration + 1) * 1000, [=]() {
                    this->send_command(Command::GET_STATUS);
                });
            } else if (door_state == DoorState::CLOSING) {
                // door started closing
                if (prev_door_state == DoorState::OPENING) {
                    this->door_position_update();
                    this->cancel_position_sync_callbacks();
                    this->door_move_delta = DOOR_DELTA_UNKNOWN;
                }
                this->door_start_moving = millis();
                this->door_start_position = *this->door_position;
                if (this->door_move_delta == DOOR_DELTA_UNKNOWN) {
                    this->door_move_delta = 0.0 - this->door_start_position;
                }
                this->schedule_door_position_sync();

                // this would only get called if no status message is received after door stops moving
                // request a status message in that case
                set_timeout("door_status_update", (*this->closing_duration + 1) * 1000, [=]() {
                    this->send_command(Command::GET_STATUS);
                });
            } else if (door_state == DoorState::STOPPED) {
                this->door_position_update();
                if (*this->door_position == DOOR_POSITION_UNKNOWN) {
                    this->door_position = 0.5; // best guess
                }
                this->cancel_position_sync_callbacks();
            } else if (door_state == DoorState::OPEN) {
                this->door_position = 1.0;
                this->cancel_position_sync_callbacks();
            } else if (door_state == DoorState::CLOSED) {
                this->door_position = 0.0;
                if (this->restore_ttc_) {
                    // GET_OPENINGS is sent when door closes, delay this tx
                    set_timeout(100, [=] {
                        if (*this->ttc_time_seconds == 0) {
                            this->turn_ttc_off();
                        } else {
                            this->set_ttc_sec(*ttc_time_seconds);
                        }
                    });
                    this->restore_ttc_ = false;
                }
            } else {
                if (*this->closing_duration == 0 || *this->opening_duration == 0 || *this->door_position == DOOR_POSITION_UNKNOWN) {
                    this->door_position = 0.5; // best guess
                }
            }

            if (door_state == DoorState::OPENING && !this->moving_to_position) {
                this->position_sync_while_opening(1.0 - *this->door_position);
                this->moving_to_position = true;
            }
            if (door_state == DoorState::CLOSING && !this->moving_to_position) {
                this->position_sync_while_closing(*this->door_position);
                this->moving_to_position = true;
            }

            if (door_state == DoorState::OPEN || door_state == DoorState::CLOSED || door_state == DoorState::STOPPED) {
                this->cancel_position_sync_callbacks();
            }

            this->door_state = door_state;
            this->light_state = static_cast<LightState>((byte2 >> 1) & 1); // safe because it can only be 0 or 1
            this->lock_state = static_cast<LockState>(byte2 & 1); // safe because it can only be 0 or 1
            this->motion_state = MotionState::CLEAR; // when the status message is read, reset motion state to 0|clear
            this->motor_state = MotorState::OFF; // when the status message is read, reset motor state to 0|off

            if (this->obstruction_from_status_) {
                // ESP_LOGD(TAG, "Obstruction: reading from byte2, bit2, status=%d", ((byte2 >> 2) & 1) == 1);
                this->obstruction_state = static_cast<ObstructionState>((byte1 >> 6) & 1);
                // This isn't very fast to update, but its still better
                // than nothing in the case the obstruction sensor is not
                // wired up.
                ESP_LOGD(TAG, "Obstruction: reading from GDO status byte1, bit6=%s", ObstructionState_to_string(*this->obstruction_state));
            }

            if (door_state == DoorState::CLOSED && door_state != prev_door_state) {
                this->send_command(Command::GET_OPENINGS);
            }

            ESP_LOGD(TAG, "Status: door=%s light=%s lock=%s",
                DoorState_to_string(*this->door_state),
                LightState_to_string(*this->light_state),
                LockState_to_string(*this->lock_state));
        } else if (cmd == Command::LIGHT) {
            if (nibble == 0) {
                this->light_state = LightState::OFF;
            } else if (nibble == 1) {
                this->light_state = LightState::ON;
            } else if (nibble == 2) { // toggle
                this->light_state = light_state_toggle(*this->light_state);
            }
            ESP_LOGD(TAG, "Light: action=%s state=%s",
                nibble == 0 ? "OFF" : nibble == 1 ? "ON"
                                                  : "TOGGLE",
                LightState_to_string(*this->light_state));
        } else if (cmd == Command::MOTOR_ON) {
            this->motor_state = MotorState::ON;
            ESP_LOGD(TAG, "Motor: state=%s", MotorState_to_string(*this->motor_state));
        } else if (cmd == Command::DOOR_ACTION) {
            this->button_state = (byte1 & 1) == 1 ? ButtonState::PRESSED : ButtonState::RELEASED;
            ESP_LOGD(TAG, "Open: button=%s", ButtonState_to_string(*this->button_state));
        } else if (cmd == Command::OPENINGS) {
            // nibble==0 if it's our request
            // update openings only from our request or if it's not unknown state
            if (nibble == 0 || *this->openings != 0) {
                this->openings = (byte1 << 8) | byte2;
                ESP_LOGD(TAG, "Openings: %d", *this->openings);
                query_status_flags_ |= QSF_OPENINGS;
            } else {
                ESP_LOGD(TAG, "Ignoreing openings, not from our request");
            }
        } else if (cmd == Command::MOTION) {
            this->motion_state = MotionState::DETECTED;
            if (*this->light_state == LightState::OFF) {
                this->send_command(Command::GET_STATUS);
            }
            ESP_LOGD(TAG, "Motion: %s", MotionState_to_string(*this->motion_state));
        } else if (cmd == Command::TTC_SET_DURATION) {
            auto seconds = (byte1 << 8) | byte2;
            ESP_LOGD(TAG, "Time to close (TTC) update request: %ds", seconds);
        } else if (cmd == Command::TTC_DURATION) {
            auto seconds = (byte1 << 8) | byte2;
            ESP_LOGD(TAG, "Time to close (TTC) set to: %ds", seconds);
            query_status_flags_ |= QSF_TCC_DUR;
            if (seconds == 60 || seconds == 300 || seconds == 600 || seconds == 0) {
                this->ttc_time_seconds = seconds;
            } else if (seconds != 1) {
                this->ttc_time_seconds = 0;
                ESP_LOGW(TAG, "Unsupported TTC time: %ds", seconds);
            }
            if (this->restore_hold_state_ == true && this->restore_ttc_ == false) {
                this->hold_enable();
                this->restore_hold_state_ = false;
            }
        } else if (cmd == Command::TTC_COUNTDOWN) {
            auto seconds = (byte1 << 8) | byte2;
            ESP_LOGD(TAG, "(TTC) door will close in: %ds", seconds);
        } else if (cmd == Command::TTC_CANCEL) {
            if (byte1 == 0x04) {
                ESP_LOGD(TAG, "TTC: Auto Hold Toggle Request");
            } else if (byte1 == 0x05) {
                ESP_LOGD(TAG, "TTC: Disable Request");
            } else {
                ESP_LOGW(TAG, "TTC_CANCEL: Unknown Data");
            }
        } else if (cmd == Command::EXT_STATUS) {
            query_status_flags_ |= QSF_EXT_STATUS;
            if (byte1 == 0x09) {
                ESP_LOGD(TAG, "TTC is disabled.");
                this->hold_state = HoldState::HOLD_DISABLED;
                this->ttc_time_seconds = 0;
            } else if (byte1 == 0x0a) {
                ESP_LOGD(TAG, "TTC is enabled but in Hold Open.");
                this->hold_state = HoldState::HOLD_ENABLED;
            } else if (byte1 == 0x0c || byte1 == 0x01) {
                ESP_LOGD(TAG, "TTC is enabled.");
                this->hold_state = HoldState::HOLD_DISABLED;
            } else if (byte1 == 0x0d || byte1 == 0x0e) {
                ESP_LOGW(TAG, "TTC closing was interrupted!");
            } else if (byte1 == 0x0b) {
                ESP_LOGD(TAG, "TTC closing now");
            }
        }

        return cmd;
    }

    void RATGDOComponent::schedule_door_position_sync(float update_period)
    {
        ESP_LOG1(TAG, "Schedule position sync: delta %f, start position: %f, start moving: %d",
            this->door_move_delta, this->door_start_position, this->door_start_moving);
        auto duration = this->door_move_delta > 0 ? *this->opening_duration : *this->closing_duration;
        auto count = int(1000 * duration / update_period);
        set_retry("position_sync_while_moving", update_period, count, [=](uint8_t r) {
            this->door_position_update();
            return RetryResult::RETRY;
        });
    }

    void RATGDOComponent::door_position_update()
    {
        if (this->door_start_moving == 0 || this->door_start_position == DOOR_POSITION_UNKNOWN || this->door_move_delta == DOOR_DELTA_UNKNOWN) {
            return;
        }
        auto now = millis();
        auto duration = this->door_move_delta > 0 ? *this->opening_duration : -*this->closing_duration;
        auto position = this->door_start_position + (now - this->door_start_moving) / (1000 * duration);
        ESP_LOG2(TAG, "[%d] Position update: %f", now, position);
        this->door_position = clamp(position, 0.0f, 1.0f);
    }

    void RATGDOComponent::encode_packet(Command command, uint32_t data, bool increment, WirePacket& packet)
    {
        auto cmd = static_cast<uint64_t>(command);
        uint64_t fixed = ((cmd & ~0xff) << 24) | this->remote_id_;
        uint32_t send_data = (data << 8) | (cmd & 0xff);

        ESP_LOG2(TAG, "[%ld] Encode for transmit rolling=%07" PRIx32 " fixed=%010" PRIx64 " data=%08" PRIx32, millis(), *this->rolling_code_counter, fixed, send_data);
        encode_wireline(*this->rolling_code_counter, fixed, send_data, packet);

        if (increment) {
            this->increment_rolling_code_counter();
        }
    }

    void RATGDOComponent::set_opening_duration(float duration)
    {
        ESP_LOGD(TAG, "Set opening duration: %.1fs", duration);
        this->opening_duration = duration;
    }

    void RATGDOComponent::set_closing_duration(float duration)
    {
        ESP_LOGD(TAG, "Set closing duration: %.1fs", duration);
        this->closing_duration = duration;
    }

    void RATGDOComponent::set_rolling_code_counter(uint32_t counter)
    {
        ESP_LOGV(TAG, "Set rolling code counter to %d", counter);
        this->rolling_code_counter = counter;
    }

    void RATGDOComponent::increment_rolling_code_counter(int delta)
    {
        this->rolling_code_counter = (*this->rolling_code_counter + delta) & 0xfffffff;
    }

    void RATGDOComponent::print_packet(const WirePacket& packet) const
    {
        ESP_LOG2(TAG, "Counter: %d Send code: [%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X]",
            *this->rolling_code_counter,
            packet[0],
            packet[1],
            packet[2],
            packet[3],
            packet[4],
            packet[5],
            packet[6],
            packet[7],
            packet[8],
            packet[9],
            packet[10],
            packet[11],
            packet[12],
            packet[13],
            packet[14],
            packet[15],
            packet[16],
            packet[17],
            packet[18]);
    }

    /*************************** OBSTRUCTION DETECTION ***************************/

    void RATGDOComponent::obstruction_loop()
    {
        long current_millis = millis();
        static unsigned long last_millis = 0;
        static unsigned long last_asleep = 0;

        // the obstruction sensor has 3 states: clear (HIGH with LOW pulse every 7ms), obstructed (HIGH), asleep (LOW)
        // the transitions between awake and asleep are tricky because the voltage drops slowly when falling asleep
        // and is high without pulses when waking up

        // If at least 3 low pulses are counted within 50ms, the door is awake, not obstructed and we don't have to check anything else

        const long CHECK_PERIOD = 50;
        const long PULSES_LOWER_LIMIT = 3;

        if (current_millis - last_millis > CHECK_PERIOD) {
            // ESP_LOGD(TAG, "%ld: Obstruction count: %d, expected: %d, since asleep: %ld",
            //     current_millis, this->isr_store_.obstruction_low_count, PULSES_EXPECTED,
            //     current_millis - last_asleep
            // );

            // check to see if we got more then PULSES_LOWER_LIMIT pulses
            if (this->isr_store_.obstruction_low_count > PULSES_LOWER_LIMIT) {
                this->obstruction_state = ObstructionState::CLEAR;
            } else if (this->isr_store_.obstruction_low_count == 0) {
                // if there have been no pulses the line is steady high or low
                if (!this->input_obst_pin_->digital_read()) {
                    // asleep
                    last_asleep = current_millis;
                } else {
                    // if the line is high and was last asleep more than 700ms ago, then there is an obstruction present
                    if (current_millis - last_asleep > 700) {
                        this->obstruction_state = ObstructionState::OBSTRUCTED;
                    }
                }
            }
            last_millis = current_millis;
            this->isr_store_.obstruction_low_count = 0;
        }
    }

    void RATGDOComponent::gdo_state_loop()
    {
        static bool reading_msg = false;
        static uint32_t msg_start = 0;
        static uint16_t byte_count = 0;
        static WirePacket rx_packet;

        if (!reading_msg) {
            while (this->sw_serial_.available()) {
                uint8_t ser_byte = this->sw_serial_.read();
                if (ser_byte != 0x55 && ser_byte != 0x01 && ser_byte != 0x00) {
                    ESP_LOG2(TAG, "Ignoring byte: %02X, baud: %d", ser_byte, this->sw_serial_.baudRate());
                    byte_count = 0;
                    continue;
                }
                msg_start = ((msg_start << 8) | ser_byte) & 0xffffff;
                byte_count++;

                // if we are at the start of a message, capture the next 16 bytes
                if (msg_start == 0x550100) {
                    rx_packet[0] = 0x55;
                    rx_packet[1] = 0x01;
                    rx_packet[2] = 0x00;

                    reading_msg = true;
                    break;
                }
            }
        }
        if (reading_msg) {
            while (this->sw_serial_.available()) {
                uint8_t ser_byte = this->sw_serial_.read();
                rx_packet[byte_count] = ser_byte;
                byte_count++;

                if (byte_count == PACKET_LENGTH) {
                    reading_msg = false;
                    byte_count = 0;
                    this->decode_packet(rx_packet);
                    return;
                }
            }
        }
    }

    void RATGDOComponent::query_status()
    {
        query_status_flags_ = 0;

        set_retry(
            750, 10, [=](uint8_t r) {
                // Once a new message is returned for each status then query_status has completed successfully
                if (query_status_flags_ == (QSF_STATUS | QSF_EXT_STATUS | QSF_TCC_DUR | QSF_OPENINGS)) {
                    ESP_LOGD(TAG, "query_status completed successfully");
                    return RetryResult::DONE;
                }
                ESP_LOGD(TAG, "query_status retry %d", 10 - r);
                // on each retry, queue up a request to GET_ each status item
                send_command(Command::GET_STATUS);
                set_timeout(150, [=] { this->send_command(Command::GET_EXT_STATUS, data::GET_EXT_STATUS); });
                set_timeout(300, [=] { this->send_command(Command::TTC_GET_DURATION, data::TTC_GET_DURATION); });
                set_timeout(450, [=] { this->send_command(Command::GET_OPENINGS); });

                if (r == 0) { // failed to sync probably rolling counter is wrong, notify
                    ESP_LOGD(TAG, "Triggering sync failed actions.");
                    this->sync_failed = true;
                };
                return RetryResult::RETRY;
            },
            1.5f);
    }

    void RATGDOComponent::query_openings()
    {
        this->query_status();
    }

    // TODO close with alert seems to get ignored right after opening door
    void RATGDOComponent::close_with_alert()
    {
        if (*this->door_state == DoorState::CLOSED) {
            ESP_LOGW(TAG, "close_with_alert door already closed!");
            return;
        }

        if (*this->door_state == DoorState::OPEN) {
            if (*this->hold_state == HoldState::HOLD_ENABLED) {
                this->restore_hold_state_ = true;
            }
            // SET_TTC closes door in 1 second with builtin gdo alert
            set_ttc_sec(1);
            this->restore_ttc_ = true;
            return;
        }

        // If not opened or closed, open the door and que to retry TTC every 1/2 second
        // TTC only works with door fully open
        open_door();
        set_timeout(500, [=] { this->close_with_alert(); });
    }

    void RATGDOComponent::turn_ttc_off()
    {
        send_command(Command::TTC_CANCEL, data::TTC_CANCEL_OFF);
    }

    void RATGDOComponent::ttc_toggle_hold()
    {
        send_command(Command::TTC_CANCEL, data::TTC_CANCEL_TOGGLE_HOLD);
    }

    void RATGDOComponent::set_ttc_sec(uint16_t duration)
    {
        send_command(Command::TTC_SET_DURATION, (duration & 0xff) << 16 | (duration & 0xff00) | 0x01);
    }

    void RATGDOComponent::send_command(Command command, uint32_t data, bool increment)
    {
        if (!this->transmit_pending_) { // have an untransmitted packet
            this->encode_packet(command, data, increment, this->tx_packet_);
        } else {
            // unlikely this would happed, we're ensuring any pending packet
            // is transmitted each loop before doing anyting else
            ESP_LOGW(TAG, "Have untransmitted packet, ignoring command: %s", Command_to_string(command));
        }
        this->transmit_packet();
    }

    bool RATGDOComponent::transmit_packet()
    {
        auto now = micros();
        while (micros() - now < 1300) {
            if (this->input_gdo_pin_->digital_read()) {
                ESP_LOGD(TAG, "Collision detected, waiting to send packet");
                this->transmit_pending_ = true;
                return false;
            }
            delayMicroseconds(100);
        }

        ESP_LOG2(TAG, "Sending packet");
        this->print_packet(this->tx_packet_);

        // indicate the start of a frame by pulling the 12V line low for at leat 1 byte followed by
        // one STOP bit, which indicates to the receiving end that the start of the message follows
        // The output pin is controlling a transistor, so the logic is inverted
        this->output_gdo_pin_->digital_write(true); // pull the line low for at least 1 byte
        delayMicroseconds(1300);
        this->output_gdo_pin_->digital_write(false); // line high for at least 1 bit
        delayMicroseconds(130);

        this->sw_serial_.write(this->tx_packet_, PACKET_LENGTH);
        this->transmit_pending_ = false;
        return true;
    }

    void RATGDOComponent::sync()
    {
        // increment rolling code counter by some amount in case we crashed without writing to flash the latest value
        this->increment_rolling_code_counter(MAX_CODES_WITHOUT_FLASH_WRITE);
        query_status();
    }

    void RATGDOComponent::open_door()
    {
        if (*this->door_state == DoorState::OPENING) {
            return; // gets ignored by opener
        }

        this->door_command(data::DOOR_OPEN);
    }

    void RATGDOComponent::close_door()
    {
        if (*this->door_state == DoorState::CLOSING || *this->door_state == DoorState::OPENING) {
            return; // gets ignored by opener
        }

        this->door_command(data::DOOR_CLOSE);
    }

    void RATGDOComponent::stop_door()
    {
        if (*this->door_state != DoorState::OPENING && *this->door_state != DoorState::CLOSING) {
            ESP_LOGW(TAG, "The door is not moving.");
            return;
        }
        this->door_command(data::DOOR_STOP);
    }

    void RATGDOComponent::toggle_door()
    {
        if (*this->door_state == DoorState::OPENING) {
            return; // gets ignored by opener
        }

        this->door_command(data::DOOR_TOGGLE);
    }

    void RATGDOComponent::door_move_to_position(float position)
    {
        if (*this->door_state == DoorState::OPENING || *this->door_state == DoorState::CLOSING) {
            ESP_LOGW(TAG, "The door is moving, ignoring.");
            return;
        }

        auto delta = position - *this->door_position;
        if (delta == 0) {
            ESP_LOGD(TAG, "Door is already at position %.2f", position);
            return;
        }

        auto duration = delta > 0 ? *this->opening_duration : -*this->closing_duration;
        if (duration == 0) {
            ESP_LOGW(TAG, "I don't know duration, ignoring move to position");
            return;
        }

        auto operation_time = 1000 * duration * delta;
        this->door_move_delta = delta;
        ESP_LOGD(TAG, "Moving to position %.2f in %.1fs", position, operation_time / 1000.0);

        this->door_command(delta > 0 ? data::DOOR_OPEN : data::DOOR_CLOSE);
        set_timeout("move_to_position", operation_time, [=] {
            this->door_command(data::DOOR_STOP);
        });
    }

    void RATGDOComponent::cancel_position_sync_callbacks()
    {
        if (this->door_start_moving != 0) {
            ESP_LOGD(TAG, "Cancelling position callbacks");
            cancel_timeout("move_to_position");
            cancel_retry("position_sync_while_moving");
            cancel_timeout("door_status_update");

            this->door_start_moving = 0;
            this->door_start_position = DOOR_POSITION_UNKNOWN;
            this->door_move_delta = DOOR_DELTA_UNKNOWN;
        }
    }

    void RATGDOComponent::door_command(uint32_t data)
    {
        data |= (1 << 16); // button 1 ?
        data |= (1 << 8); // button press
        this->send_command(Command::DOOR_ACTION, data, false);
        set_timeout(200, [=] {
            auto data2 = data & ~(1 << 8); // button release
            this->send_command(Command::DOOR_ACTION, data2);
        });
    }

    void RATGDOComponent::light_on()
    {
        this->light_state = LightState::ON;
        this->send_command(Command::LIGHT, data::LIGHT_ON);
    }

    void RATGDOComponent::light_off()
    {
        this->light_state = LightState::OFF;
        this->send_command(Command::LIGHT, data::LIGHT_OFF);
    }

    void RATGDOComponent::toggle_light()
    {
        this->light_state = light_state_toggle(*this->light_state);
        this->send_command(Command::LIGHT, data::LIGHT_TOGGLE);
    }

    // Lock functions
    void RATGDOComponent::lock()
    {
        this->lock_state = LockState::LOCKED;
        this->send_command(Command::LOCK, data::LOCK_ON);
    }

    void RATGDOComponent::unlock()
    {
        this->lock_state = LockState::UNLOCKED;
        this->send_command(Command::LOCK, data::LOCK_OFF);
    }

    void RATGDOComponent::toggle_lock()
    {
        this->lock_state = lock_state_toggle(*this->lock_state);
        this->send_command(Command::LOCK, data::LOCK_TOGGLE);
    }

    // Hold functions
    void RATGDOComponent::hold_enable()
    {
        if (*(this->hold_state) == HoldState::HOLD_DISABLED) {
            this->toggle_hold();
        }
    }

    void RATGDOComponent::hold_disable()
    {
        if (*(this->hold_state) == HoldState::HOLD_ENABLED) {
            this->toggle_hold();
        }
    }

    void RATGDOComponent::toggle_hold()
    {
        this->hold_state = hold_state_toggle(*this->hold_state);
        this->send_command(Command::TTC_CANCEL, data::TTC_CANCEL_TOGGLE_HOLD);
    }

    LightState RATGDOComponent::get_light_state() const
    {
        return *this->light_state;
    }

    void RATGDOComponent::subscribe_rolling_code_counter(std::function<void(uint32_t)>&& f)
    {
        // change update to children is defered until after component loop
        // if multiple changes occur during component loop, only the last one is notified
        this->rolling_code_counter.subscribe([=](uint32_t state) { defer("rolling_code_counter", [=] { f(state); }); });
    }
    void RATGDOComponent::subscribe_opening_duration(std::function<void(float)>&& f)
    {
        this->opening_duration.subscribe([=](float state) { defer("opening_duration", [=] { f(state); }); });
    }
    void RATGDOComponent::subscribe_closing_duration(std::function<void(float)>&& f)
    {
        this->closing_duration.subscribe([=](float state) { defer("closing_duration", [=] { f(state); }); });
    }
    void RATGDOComponent::subscribe_openings(std::function<void(uint16_t)>&& f)
    {
        this->openings.subscribe([=](uint16_t state) { defer("openings", [=] { f(state); }); });
    }
    void RATGDOComponent::subscribe_door_state(std::function<void(DoorState, float)>&& f)
    {
        this->door_state.subscribe([=](DoorState state) {
            defer("door_state", [=] { f(state, *this->door_position); });
        });
        this->door_position.subscribe([=](float position) {
            defer("door_state", [=] { f(*this->door_state, position); });
        });
    }
    void RATGDOComponent::subscribe_light_state(std::function<void(LightState)>&& f)
    {
        this->light_state.subscribe([=](LightState state) { defer("light_state", [=] { f(state); }); });
    }
    void RATGDOComponent::subscribe_lock_state(std::function<void(LockState)>&& f)
    {
        this->lock_state.subscribe([=](LockState state) { defer("lock_state", [=] { f(state); }); });
    }
    void RATGDOComponent::subscribe_hold_state(std::function<void(HoldState)>&& f)
    {
        this->hold_state.subscribe([=](HoldState state) { defer("hold_state", [=] { f(state); }); });
    }
    void RATGDOComponent::subscribe_ttc_seconds(std::function<void(uint16_t)>&& f)
    {
        this->ttc_time_seconds.subscribe([=](uint16_t state) { defer("ttc_time", [=] { f(state); }); });
    }
    void RATGDOComponent::subscribe_obstruction_state(std::function<void(ObstructionState)>&& f)
    {
        this->obstruction_state.subscribe([=](ObstructionState state) { defer("obstruction_state", [=] { f(state); }); });
    }
    void RATGDOComponent::subscribe_motor_state(std::function<void(MotorState)>&& f)
    {
        this->motor_state.subscribe([=](MotorState state) { defer("motor_state", [=] { f(state); }); });
    }
    void RATGDOComponent::subscribe_button_state(std::function<void(ButtonState)>&& f)
    {
        this->button_state.subscribe([=](ButtonState state) { defer("button_state", [=] { f(state); }); });
    }
    void RATGDOComponent::subscribe_motion_state(std::function<void(MotionState)>&& f)
    {
        this->motion_state.subscribe([=](MotionState state) { defer("motion_state", [=] { f(state); }); });
    }
    void RATGDOComponent::subscribe_sync_failed(std::function<void(bool)>&& f)
    {
        this->sync_failed.subscribe(std::move(f));
    }

} // namespace ratgdo
} // namespace esphome
