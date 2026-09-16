`include "i2c_master.vh"

// ===================================================================
// i2c_master -- a generic I2C bus master, built from scratch in raw
// Verilog (no vendor IP, no library). This file knows NOTHING about what
// chip is on the other end of the bus -- no sensor registers, no specific
// addresses. It only knows how to speak the I2C protocol itself: START,
// send/receive a byte, STOP. Any project talking to any I2C peripheral can
// reuse this file unmodified; only the code that DRIVES it (like hello.v)
// needs to know about the specific sensor.
//
// -------------------------------------------------------------------
// I2C protocol, the short version, for readers new to it:
// -------------------------------------------------------------------
// I2C is a 2-wire bus: SDA (data) and SCL (clock). Multiple chips share
// the same two wires. Both wires are "open-drain": a chip can only pull a
// wire LOW, never drive it HIGH. An external pull-up resistor is what
// actually makes the wire read HIGH when nobody is pulling it low. This
// matters a lot for how the Verilog below is written (see "open-drain
// I2C lines" further down) -- notice we never do `assign SDA = 1'b1`.
//
// A transaction on the bus looks like this:
//   1. START condition: SDA falls while SCL is held high. This is the
//      universal "attention, I'm about to talk" signal -- it's what makes
//      START different from a normal data bit (during normal bits, SDA is
//      only allowed to change while SCL is LOW).
//   2. One or more bytes, MSB (most-significant bit) first. After each
//      8 data bits, the sender releases SDA and the RECEIVER pulls SDA low
//      for one extra clock pulse to say "got it" -- that's the ACK bit.
//      If the receiver leaves SDA high instead, that's a NACK (didn't
//      acknowledge -- e.g. wrong address, or "no more data please" on a
//      read).
//   3. The very first byte after START is always the target chip's 7-bit
//      address plus one bit saying whether this is a write (0) or a read
//      (1) -- that's why you'll see e.g. address 0x3A show up as two
//      different bytes, 0x74 (write) and 0x75 (read), in hello.v.
//   4. STOP condition: SDA rises while SCL is held high -- the mirror
//      image of START, and it means "I'm done, bus is free."
// This engine below executes exactly one of START / one-byte / STOP each
// time it's told to "go" -- the calling code (hello.v) is responsible for
// stringing those together into a full transaction.
// ===================================================================
module i2c_master #(
    parameter QUARTER          = 40,        // CLK cycles per SCL quarter-period
    parameter SCL_WAIT_TIMEOUT = 16'd16000  // cycles to wait for SCL to read back high before giving up
) (
    input            CLK,
    inout            SDA,  // I2C data (open-drain, needs pull-up)
    inout            SCL,  // I2C clock (open-drain, needs pull-up)

    // --- control inputs: the caller sets these up, then pulses `go` ---
    input      [1:0] cmd,       // `I2C_CMD_START / `I2C_CMD_BYTE / `I2C_CMD_STOP
    input            go,        // pulse 1 cycle to launch cmd (only while !busy)
    input      [7:0] tx_byte,   // CMD_BYTE write: byte to send, latched at go
    input            is_read,   // CMD_BYTE: 0 = write tx_byte, 1 = read into rx_byte
    input            send_ack,  // CMD_BYTE read: 1 = ACK (more bytes follow), 0 = NACK (last byte)

    // --- status/result outputs: the caller watches these ---
    output           busy,
    output reg       done,      // pulses 1 cycle when cmd completes
    output     [7:0] rx_byte,   // CMD_BYTE read: holds received byte once done pulses
    output reg       rx_ack     // CMD_BYTE write: slave's ack bit, valid once done pulses
);

    // -----------------------------------------------------------------
    // Open-drain I2C lines
    // -----------------------------------------------------------------
    // `sda_low`/`scl_low` are the only things this engine ever decides:
    // "am I pulling this wire low right now, or am I leaving it alone?"
    // We NEVER drive a 1 -- when we're not pulling low, the line is set to
    // 1'bz ("high-impedance" / disconnected), and the external pull-up
    // resistor is what lets it float back up to a real HIGH. This is what
    // "open-drain" means in hardware, and it's also exactly what allows
    // another chip to hold the line low itself (an ACK bit, or clock
    // stretching) even while we've "released" it.
    reg sda_low = 0;
    reg scl_low = 0;
    assign SDA = sda_low ? 1'b0 : 1'bz;
    assign SCL = scl_low ? 1'b0 : 1'bz;
    // To actually READ the current state of the wire (e.g. did the slave
    // pull SDA low for an ACK? has SCL actually risen yet?) we just read
    // the pin directly -- these wires always reflect the real, physical,
    // resistor-pulled-up-or-somebody-pulled-down voltage on the bus.
    wire sda_in = SDA;
    wire scl_in = SCL;

    // These three states describe which "shape" of waveform the engine is
    // currently drawing on the bus -- they're internal bookkeeping, not
    // something the outside world needs to know about (unlike `cmd`,
    // which is the caller's REQUEST; `eng_state` is what the engine
    // actually latches internally in response to that request).
    localparam E_START = 2'd0;
    localparam E_BIT   = 2'd1;
    localparam E_STOP  = 2'd2;

    reg [7:0]  shift_out;  // byte to send, loaded from tx_byte at go
    reg [7:0]  shift_in;
    assign rx_byte = shift_in;

    reg        eng_busy = 0;
    assign busy = eng_busy;

    reg [1:0]  eng_state;
    reg [3:0]  bit_idx;   // which of the 9 bits (8 data + 1 ack) we're on
    // `qcnt` counts CLK cycles up to `QUARTER`, i.e. it's the engine's
    // internal stopwatch for "how long is one quarter of an SCL period."
    // It's declared much wider (24 bits) than the real 100kHz setting
    // needs (QUARTER=40 would fit in 6 bits) because this same file also
    // gets used with a hugely slowed-down QUARTER value for visual
    // debugging (watching bits blink out on an LED one at a time) --
    // 24 bits keeps that working without editing this file.
    reg [23:0] qcnt;
    reg [1:0]  phase; // which quarter-period (0,1,2,3) we're in, within the current bit/start/stop

    // -----------------------------------------------------------------
    // Why we wait for SCL to actually read back HIGH (instead of just
    // trusting a fixed cycle count):
    // -----------------------------------------------------------------
    // We only ever pull SCL low or release it -- we never force it high.
    // Two real-world things can delay it from actually reaching a high
    // voltage after we release it:
    //   1. "Clock stretching" -- the I2C spec allows a SLAVE device to
    //      hold SCL low on purpose, telling the master "wait, I'm not
    //      ready yet." A well-behaved master must notice this and simply
    //      pause until the slave lets go.
    //   2. Plain RC rise time -- the pull-up resistor and the bus's wire
    //      capacitance form an RC circuit, so the voltage climbs rather
    //      than snapping instantly from 0V to high. On real hardware this
    //      takes a small but nonzero amount of time.
    // A real I2C master chip (like the hardware TWI peripheral inside an
    // AVR) actively watches the SCL pin and waits for it to read high
    // before treating the clock edge as having "happened." If we instead
    // just counted a fixed number of cycles and assumed SCL was high by
    // then, a real sensor doing clock stretching -- or just a slower rise
    // time than we guessed -- could desync the whole transaction, even
    // though from the FPGA's own point of view everything looked timed
    // correctly.
    reg        waiting_scl_high = 0;
    reg [15:0] scl_wait_cnt     = 0;

    // `tick` fires for exactly one cycle every time `qcnt` finishes
    // counting up to one quarter-period -- it's what advances `phase`.
    wire tick = (qcnt == QUARTER - 1);

    always @(posedge CLK) begin
        // `done` should only ever be high for the single cycle a command
        // finishes on, so we default it low every cycle up front, and let
        // the specific finishing branches below pulse it back to 1.
        done <= 1'b0;

        if (go) begin
            // Caller just asked us to start a brand new command. Reset
            // all our internal counters/state and latch in whatever the
            // caller has put on tx_byte right now (tx_byte is only
            // guaranteed valid at the moment `go` is pulsed).
            qcnt             <= 0;
            phase            <= 0;
            bit_idx          <= 0;
            shift_out        <= tx_byte;
            eng_busy         <= 1'b1;
            waiting_scl_high <= 1'b0;
            eng_state        <= (cmd == `I2C_CMD_START) ? E_START :
                                 (cmd == `I2C_CMD_STOP)  ? E_STOP  : E_BIT;
        end else if (eng_busy) begin
            if (waiting_scl_high) begin
                // Paused after releasing SCL, waiting for the bus to
                // actually read back high (see the big comment above).
                // `scl_wait_cnt >= SCL_WAIT_TIMEOUT` is just a safety net
                // so a stuck bus can't hang the engine forever.
                if (scl_in || scl_wait_cnt >= SCL_WAIT_TIMEOUT) begin
                    waiting_scl_high <= 1'b0;
                    qcnt             <= 0; // fresh timing window now SCL is confirmed high
                end else begin
                    scl_wait_cnt <= scl_wait_cnt + 1'b1;
                end
            end else if (!tick) begin
                // Still counting up through the current quarter-period --
                // nothing to do yet but keep counting.
                qcnt <= qcnt + 1'b1;
            end else begin
                // A quarter-period just elapsed: move to the next phase
                // and let the current eng_state decide what that means.
                qcnt  <= 0;
                phase <= phase + 1'b1;

                case (eng_state)
                    E_START: begin
                        // A START condition is: make sure SDA is released
                        // and SCL is high, THEN pull SDA low while SCL
                        // stays high (that falling edge on SDA, while SCL
                        // is high, is the START signal itself -- it's
                        // what makes this different from a normal data
                        // bit change, which is only allowed while SCL is
                        // low). Writing it as "release SCL, wait for it
                        // to really be high, THEN pull SDA low" makes this
                        // work correctly whether SCL was already high
                        // (very first START) or still low from a previous
                        // byte (a "repeated START" mid-transaction).
                        case (phase)
                            2'd0: begin sda_low <= 1'b0; scl_low <= 1'b1; end
                            2'd1: begin scl_low <= 1'b0; waiting_scl_high <= 1'b1; scl_wait_cnt <= 0; end
                            2'd2: begin sda_low <= 1'b1; end // START edge (SDA falls while SCL high)
                            2'd3: begin scl_low <= 1'b1; eng_busy <= 1'b0; done <= 1'b1; end
                        endcase
                    end

                    E_BIT: begin
                        // One pass through this case handles ONE bit time
                        // -- either a data bit (bit_idx 0..7) or the 9th
                        // "bit," the ack slot (bit_idx == 8). It runs 9
                        // times in a row (driven by the sequencer re-
                        // issuing CMD_BYTE... no, actually: bit_idx counts
                        // 0..8 internally within a single CMD_BYTE command,
                        // so ONE `go` pulse with cmd=CMD_BYTE sends/reads
                        // the whole 8 data bits PLUS the ack bit before
                        // pulsing `done`.
                        case (phase)
                            2'd0: begin
                                // Phase 0: SCL is low (safe to change SDA
                                // here, per the I2C rule that data may
                                // only change while the clock is low).
                                // Put the next bit we want to send onto
                                // SDA now, before we raise SCL.
                                scl_low <= 1'b1;
                                if (bit_idx == 4'd8) begin
                                    // This is the ack slot. If WE are the
                                    // one reading (is_read), we're the one
                                    // who owes an ack/nack back to the
                                    // slave. If we were writing, it's the
                                    // SLAVE's turn to ack, so we must
                                    // release SDA and just listen.
                                    sda_low <= is_read ? send_ack : 1'b0; // ack/nack, or release for slave's ack
                                end else begin
                                    // Normal data bit: if we're reading,
                                    // we must release SDA so the slave can
                                    // drive it. If we're writing, drive
                                    // out the next bit of shift_out
                                    // (MSB first, hence bit [7]).
                                    // `sda_low` is active-LOW logic (1
                                    // means "pull the wire low"), which is
                                    // why sending a data bit of 1 means
                                    // sda_low = 0 (i.e. ~shift_out[7]).
                                    sda_low <= is_read ? 1'b0 : ~shift_out[7];
                                end
                            end
                            2'd1: begin end // setup time -- let SDA settle before SCL rises
                            2'd2: begin scl_low <= 1'b0; waiting_scl_high <= 1'b1; scl_wait_cnt <= 0; end // SCL rises, data valid
                            2'd3: begin
                                // Phase 3: SCL has been high for a bit --
                                // this is our sampling moment, and also
                                // where we decide whether this command is
                                // finished (after the ack bit) or whether
                                // there's another bit to go.
                                if (bit_idx == 4'd8) begin
                                    // Just finished the ack bit. If we
                                    // were writing, this is where we find
                                    // out whether the slave acked us --
                                    // sample SDA right now and remember
                                    // it as rx_ack for the caller to check
                                    // once `done` pulses.
                                    if (!is_read) rx_ack <= sda_in;
                                    scl_low  <= 1'b1; // close out the ack bit's clock pulse
                                    eng_busy <= 1'b0;
                                    done     <= 1'b1;
                                end else begin
                                    // Still in the 8 data bits: capture
                                    // the bit we just read (if reading),
                                    // shift our outgoing byte over by one
                                    // (if writing), and move to the next
                                    // bit index.
                                    if (is_read) shift_in <= {shift_in[6:0], sda_in};
                                    shift_out <= {shift_out[6:0], 1'b0};
                                    bit_idx   <= bit_idx + 1'b1;
                                end
                            end
                        endcase
                    end

                    E_STOP: begin
                        // A STOP condition is the mirror image of START:
                        // pull SDA low first (while SCL is still low, so
                        // it's a legal, quiet change), raise SCL, THEN
                        // release SDA while SCL is high -- that rising
                        // edge on SDA, while SCL is high, is the STOP
                        // signal ("I'm done, bus is free for anyone").
                        case (phase)
                            2'd0: begin scl_low <= 1'b1; sda_low <= 1'b1; end
                            2'd1: begin scl_low <= 1'b0; waiting_scl_high <= 1'b1; scl_wait_cnt <= 0; end
                            2'd2: begin sda_low <= 1'b0; end // STOP edge (SDA rises while SCL high)
                            2'd3: begin eng_busy <= 1'b0; done <= 1'b1; end
                        endcase
                    end
                    default: ;
                endcase
            end
        end
    end

endmodule
