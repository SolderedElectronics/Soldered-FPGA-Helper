`include "i2c_master.vh"

// ===================================================================
// hello -- top-level FPGA design for this board (the "top module" apio
// synthesizes, per apio.ini). This is the ONLY file that knows about the
// specific sensor wired up (an LTR-507ALS-01 ambient light sensor): its
// I2C address, its register map, and the sequence of I2C transactions
// needed to turn it on and read a light level from it. The actual I2C
// wire-wiggling is delegated to the reusable `i2c_master` module (see
// i2c_master.v) -- this file just tells that engine what to send, one
// command at a time, and reacts to what comes back.
//
// Big picture of what this chip does, end to end:
//   1. Wait ~150ms after power-up for the sensor to be ready (datasheet
//      requirement).
//   2. Send it a one-time "wake up and start measuring" write.
//   3. Forever after: every ~500ms, read its current light measurement,
//      and use that to control how fast the onboard LED blinks (faster
//      blink = brighter light).
// ===================================================================
module hello #(
    parameter QUARTER        = 40,          // 16MHz / (100kHz*4) -> 100kHz SCL
    parameter STARTUP_CYCLES = 24'd2_400_000, // 150ms power-up wait (datasheet min 100ms)
    parameter LOOP_CYCLES    = 24'd8_000_000  // 500ms between reads (matches ALS_MEAS_RATE default)
) (
    input  CLK,  // 16MHz onboard oscillator
    output LED,  // onboard USR LED, blinks faster with more light
    inout  SDA,  // I2C data (open-drain, needs pull-up)
    inout  SCL   // I2C clock (open-drain, needs pull-up)
);

    // -----------------------------------------------------------------
    // LTR-507ALS-01 constants (SEL tied GND -> 7-bit addr 0x3A)
    // -----------------------------------------------------------------
    // I2C addresses on the wire are actually 8 bits: the chip's 7-bit
    // address, shifted left by one, with the bottom bit saying read (1)
    // or write (0). So this sensor's 7-bit address 0x3A becomes two
    // different byte values depending on what we're about to do:
    //   0x3A << 1 | 0 = 0x74 -- "I'm about to WRITE to you"
    //   0x3A << 1 | 1 = 0x75 -- "I'm about to READ from you"
    // REG_ALS_CONTR and REG_ALS_DATA0 are register addresses INSIDE the
    // sensor -- like a memory address you write first to say "I want to
    // talk about this particular setting/reading now."
    localparam I2C_ADDR_W    = 8'h74;
    localparam I2C_ADDR_R    = 8'h75;
    localparam REG_ALS_CONTR = 8'h80; // "control" register -- power mode, gain
    localparam REG_ALS_DATA0 = 8'h88; // first of two light-reading data registers
    localparam ALS_ACTIVE    = 8'h02; // value to write to REG_ALS_CONTR: ALS_MODE=1 (active), default gain

    // -----------------------------------------------------------------
    // Generic I2C master engine (see i2c_master.v) -- this module only
    // drives its command/data ports and sequences the LTR-507 protocol.
    // Everything below is just wiring: these regs/wires are how THIS
    // module (the sequencer, further down) talks to the reusable engine.
    // Think of it like a remote control -- we set tx_byte/is_read/etc,
    // press "go" for one cycle, then wait for "done" to come back.
    // -----------------------------------------------------------------
    reg  [1:0] eng_cmd;
    reg        eng_go   = 0;
    reg  [7:0] tx_byte;   // byte to send, loaded by sequencer, latched by engine at eng_go
    reg        is_read;
    reg        send_ack; // 1 = ACK (drive low) after read byte, 0 = NACK (release)

    wire       eng_done;
    wire [7:0] rx_byte;
    wire       rx_ack;   // slave's ack, valid after a write byte

    i2c_master #(
        .QUARTER (QUARTER)
    ) i2c (
        .CLK      (CLK),
        .SDA      (SDA),
        .SCL      (SCL),
        .cmd      (eng_cmd),
        .go       (eng_go),
        .tx_byte  (tx_byte),
        .is_read  (is_read),
        .send_ack (send_ack),
        .busy     (),        // unused here -- we track progress via `eng_done` and our own seq_state instead
        .done     (eng_done),
        .rx_byte  (rx_byte),
        .rx_ack   (rx_ack)
    );

    // -----------------------------------------------------------------
    // Top sequencer: one-time sensor init, then repeated ALS reads
    // -----------------------------------------------------------------
    // This is a classic "state machine": `seq_state` is a single register
    // holding a number that says "which step of the recipe are we on
    // right now," and the `case` statement below is the recipe itself --
    // one branch per step. Every state pair here follows the same
    // pattern: an "ISSUE" state that loads up one command and pulses
    // `eng_go` for exactly one clock cycle, followed by a "WAIT" state
    // that just sits there (potentially for many, many clock cycles)
    // until the engine reports back `eng_done`, then moves on to the
    // next step. This is necessary because sending even one I2C bit takes
    // many CLK cycles (see QUARTER in i2c_master.v) -- the sequencer has
    // to patiently wait, it can't just fire off commands back to back.
    localparam SEQ_POWERUP        = 0;  // waiting out STARTUP_CYCLES after power-on
    localparam SEQ_I_START_ISSUE  = 1;  // \
    localparam SEQ_I_START_WAIT   = 2;  //  > one-time init transaction: START,
    localparam SEQ_I_ADDR_ISSUE   = 3;  //  | write address (I2C_ADDR_W),
    localparam SEQ_I_ADDR_WAIT    = 4;  //  | write REG_ALS_CONTR (the register
    localparam SEQ_I_REG_ISSUE    = 5;  //  | pointer we want to write to),
    localparam SEQ_I_REG_WAIT     = 6;  //  | write ALS_ACTIVE (the actual value,
    localparam SEQ_I_DATA_ISSUE   = 7;  //  | turning the sensor on), STOP.
    localparam SEQ_I_DATA_WAIT    = 8;  //  | This only ever runs once, right
    localparam SEQ_I_STOP_ISSUE   = 9;  //  | after power-up.
    localparam SEQ_I_STOP_WAIT    = 10; // /
    localparam SEQ_LOOP_WAIT      = 11; // waiting out LOOP_CYCLES between reads (loops back here forever)
    localparam SEQ_R_START_ISSUE  = 12; // \
    localparam SEQ_R_START_WAIT   = 13; //  |
    localparam SEQ_R_ADDRW_ISSUE  = 14; //  | Repeating read transaction: START,
    localparam SEQ_R_ADDRW_WAIT   = 15; //  | write address, write REG_ALS_DATA0
    localparam SEQ_R_REG_ISSUE    = 16; //  | (pointing at the light-level
    localparam SEQ_R_REG_WAIT     = 17; //  | registers), a REPEATED START (no
    localparam SEQ_R_RSTART_ISSUE = 18; //  | STOP in between -- this is how I2C
    localparam SEQ_R_RSTART_WAIT  = 19; //  | says "same transaction, but now I
    localparam SEQ_R_ADDRR_ISSUE  = 20; //  | want to switch to reading"), write
    localparam SEQ_R_ADDRR_WAIT   = 21; //  | the address again but as a READ,
    localparam SEQ_R_DATA0_ISSUE  = 22; //  | then read back 2 bytes (low byte,
    localparam SEQ_R_DATA0_WAIT   = 23; //  | then high byte -- the datasheet
    localparam SEQ_R_DATA1_ISSUE  = 24; //  | defines this register as 16-bit,
    localparam SEQ_R_DATA1_WAIT   = 25; //  | little-endian), then STOP. Then
    localparam SEQ_R_STOP_ISSUE   = 26; //  | loop back to SEQ_LOOP_WAIT and do
    localparam SEQ_R_STOP_WAIT    = 27; // /  it all again, forever.

    reg [4:0]  seq_state = SEQ_POWERUP;
    reg [23:0] wait_cnt  = 0;         // generic cycle-counter, reused by both wait states above
    reg [7:0]  lux_low, lux_high;     // the two raw bytes read back from the sensor, before combining
    reg [15:0] lux = 0;               // combined 16-bit light reading (raw ADC counts, not real "lux" units)
    reg        sensor_acked    = 0;   // DIAGNOSTIC: updated every loop from the addr+W ack
    reg        init_addr_acked = 0;   // DIAGNOSTIC: one-shot, from the startup init write
    reg        init_reg_acked  = 0;
    reg        init_data_acked = 0;

    always @(posedge CLK) begin
        // `eng_go` must only be high for exactly the one cycle we want to
        // launch a command -- default it low every cycle, and let each
        // ISSUE state below pulse it back to 1 just for that cycle. This
        // is why `eng_go <= 1'b1;` inside an ISSUE state doesn't leave it
        // stuck on: the very next clock edge, this line resets it again
        // (unless another ISSUE state happens to set it again that same
        // cycle, which never happens here since only one state runs at a
        // time).
        eng_go <= 1'b0; // default; each ISSUE state pulses it for one cycle

        case (seq_state)
            SEQ_POWERUP: begin
                // Just count CLK cycles until STARTUP_CYCLES have passed,
                // then move on. `wait_cnt` is a plain up-counter reused
                // by SEQ_LOOP_WAIT further down too -- since only one of
                // those two states is ever active at once, they can
                // safely share the same register.
                if (wait_cnt >= STARTUP_CYCLES - 1) begin
                    wait_cnt  <= 0;
                    seq_state <= SEQ_I_START_ISSUE;
                end else begin
                    wait_cnt <= wait_cnt + 1'b1;
                end
            end

            SEQ_I_START_ISSUE: begin
                eng_cmd   <= `I2C_CMD_START;
                eng_go    <= 1'b1;
                seq_state <= SEQ_I_START_WAIT;
            end
            SEQ_I_START_WAIT: if (eng_done) seq_state <= SEQ_I_ADDR_ISSUE;

            SEQ_I_ADDR_ISSUE: begin
                tx_byte   <= I2C_ADDR_W;
                is_read   <= 1'b0;
                eng_cmd   <= `I2C_CMD_BYTE;
                eng_go    <= 1'b1;
                seq_state <= SEQ_I_ADDR_WAIT;
            end
            SEQ_I_ADDR_WAIT: if (eng_done) begin
                // rx_ack is active-LOW (0 = the sensor pulled the line
                // down = ACK), so we store the more intuitive !rx_ack
                // ("1 = it acked") into our diagnostic flag.
                init_addr_acked <= !rx_ack;
                seq_state       <= SEQ_I_REG_ISSUE;
            end

            SEQ_I_REG_ISSUE: begin
                tx_byte   <= REG_ALS_CONTR;
                is_read   <= 1'b0;
                eng_cmd   <= `I2C_CMD_BYTE;
                eng_go    <= 1'b1;
                seq_state <= SEQ_I_REG_WAIT;
            end
            SEQ_I_REG_WAIT: if (eng_done) begin
                init_reg_acked <= !rx_ack;
                seq_state      <= SEQ_I_DATA_ISSUE;
            end

            SEQ_I_DATA_ISSUE: begin
                tx_byte   <= ALS_ACTIVE;
                is_read   <= 1'b0;
                eng_cmd   <= `I2C_CMD_BYTE;
                eng_go    <= 1'b1;
                seq_state <= SEQ_I_DATA_WAIT;
            end
            SEQ_I_DATA_WAIT: if (eng_done) begin
                init_data_acked <= !rx_ack;
                seq_state       <= SEQ_I_STOP_ISSUE;
            end

            SEQ_I_STOP_ISSUE: begin
                eng_cmd   <= `I2C_CMD_STOP;
                eng_go    <= 1'b1;
                seq_state <= SEQ_I_STOP_WAIT;
            end
            SEQ_I_STOP_WAIT: if (eng_done) seq_state <= SEQ_LOOP_WAIT;

            SEQ_LOOP_WAIT: begin
                // Same idea as SEQ_POWERUP: just wait out LOOP_CYCLES
                // (~500ms) then go do another read. This is where the
                // "forever" part of the read loop actually lives -- every
                // path through the read transaction below eventually
                // lands back here.
                if (wait_cnt >= LOOP_CYCLES - 1) begin
                    wait_cnt  <= 0;
                    seq_state <= SEQ_R_START_ISSUE;
                end else begin
                    wait_cnt <= wait_cnt + 1'b1;
                end
            end

            SEQ_R_START_ISSUE: begin
                eng_cmd   <= `I2C_CMD_START;
                eng_go    <= 1'b1;
                seq_state <= SEQ_R_START_WAIT;
            end
            SEQ_R_START_WAIT: if (eng_done) seq_state <= SEQ_R_ADDRW_ISSUE;

            SEQ_R_ADDRW_ISSUE: begin
                tx_byte   <= I2C_ADDR_W;
                is_read   <= 1'b0;
                eng_cmd   <= `I2C_CMD_BYTE;
                eng_go    <= 1'b1;
                seq_state <= SEQ_R_ADDRW_WAIT;
            end
            SEQ_R_ADDRW_WAIT: if (eng_done) begin
                sensor_acked <= !rx_ack; // rx_ack=0 means the sensor drove ACK
                seq_state    <= SEQ_R_REG_ISSUE;
            end

            SEQ_R_REG_ISSUE: begin
                tx_byte   <= REG_ALS_DATA0;
                is_read   <= 1'b0;
                eng_cmd   <= `I2C_CMD_BYTE;
                eng_go    <= 1'b1;
                seq_state <= SEQ_R_REG_WAIT;
            end
            SEQ_R_REG_WAIT: if (eng_done) seq_state <= SEQ_R_RSTART_ISSUE;

            SEQ_R_RSTART_ISSUE: begin
                // Note: this is another CMD_START, not a special
                // "repeated start" command -- the i2c_master engine's
                // START handling already works correctly whether SCL was
                // left high (very first start of a transaction) or low
                // (mid-transaction, like here), so the exact same command
                // does the right thing in both places.
                eng_cmd   <= `I2C_CMD_START;
                eng_go    <= 1'b1;
                seq_state <= SEQ_R_RSTART_WAIT;
            end
            SEQ_R_RSTART_WAIT: if (eng_done) seq_state <= SEQ_R_ADDRR_ISSUE;

            SEQ_R_ADDRR_ISSUE: begin
                tx_byte   <= I2C_ADDR_R; // same 7-bit address as before, but read bit set this time
                is_read   <= 1'b0;       // we're still WRITING this address byte itself
                eng_cmd   <= `I2C_CMD_BYTE;
                eng_go    <= 1'b1;
                seq_state <= SEQ_R_ADDRR_WAIT;
            end
            SEQ_R_ADDRR_WAIT: if (eng_done) seq_state <= SEQ_R_DATA0_ISSUE;

            SEQ_R_DATA0_ISSUE: begin
                is_read   <= 1'b1;
                send_ack  <= 1'b1; // ACK, one more byte to come
                eng_cmd   <= `I2C_CMD_BYTE;
                eng_go    <= 1'b1;
                seq_state <= SEQ_R_DATA0_WAIT;
            end
            SEQ_R_DATA0_WAIT: if (eng_done) begin
                lux_low   <= rx_byte;
                seq_state <= SEQ_R_DATA1_ISSUE;
            end

            SEQ_R_DATA1_ISSUE: begin
                is_read   <= 1'b1;
                send_ack  <= 1'b0; // NACK, last byte -- tells the sensor "stop, don't send more"
                eng_cmd   <= `I2C_CMD_BYTE;
                eng_go    <= 1'b1;
                seq_state <= SEQ_R_DATA1_WAIT;
            end
            SEQ_R_DATA1_WAIT: if (eng_done) begin
                lux_high  <= rx_byte;
                seq_state <= SEQ_R_STOP_ISSUE;
            end

            SEQ_R_STOP_ISSUE: begin
                eng_cmd   <= `I2C_CMD_STOP;
                eng_go    <= 1'b1;
                seq_state <= SEQ_R_STOP_WAIT;
            end
            SEQ_R_STOP_WAIT: if (eng_done) begin
                // Combine the two bytes into one 16-bit reading now that
                // both have arrived. `{lux_high, lux_low}` is Verilog's
                // concatenation syntax -- it just glues the bits of
                // lux_high and lux_low together into one wider value,
                // high byte first (this matches the sensor's own
                // low-byte-first register layout, since we read DATA0
                // before DATA1/high).
                lux       <= {lux_high, lux_low};
                seq_state <= SEQ_LOOP_WAIT;
            end

            default: seq_state <= SEQ_POWERUP; // shouldn't happen, but recover cleanly if it ever does
        endcase
    end

    // -----------------------------------------------------------------
    // Blink-rate output: faster blink with higher lux, clamped
    // -----------------------------------------------------------------
    // The idea: the LED toggles on/off every `period` clock cycles.
    // A SMALL period means the LED toggles often -- fast blinking.
    // A LARGE period means it toggles rarely -- slow blinking.
    // So "faster blink in bright light" just means: as `lux` goes up,
    // `period` should go DOWN. We compute that by starting at the
    // slowest allowed period and subtracting more and more from it as
    // light increases, clamped so it never goes below the fastest
    // allowed period.
    localparam [24:0] PERIOD_MAX   = 25'd32_000_000; // ~2s half-period (slow, dark)
    localparam [24:0] PERIOD_MIN   = 25'd100_000;    // ~6.25ms half-period (very fast, bright)
    localparam [24:0] DELTA_RANGE  = PERIOD_MAX - PERIOD_MIN; // the whole span we're allowed to subtract

    // lux << 12 crosses DELTA_RANGE around lux ~ 7700-10000, i.e. "full
    // speed" lands in the requested 0-10000 range (same crossover as before
    // PERIOD_MAX grew -- shift widened from 10 to 12 to compensate). Pure
    // shift, not a multiply/divide -- iCE40 LP8K has no hardware
    // multiplier, so a wide combinational multiply here blows the 16MHz
    // timing budget. A left-shift by N is exactly the same math as
    // multiplying by 2^N, but it synthesizes to free wiring (just moving
    // bits over) instead of a multiplier circuit -- that's the trick.
    wire [31:0] lux_ext       = {16'd0, lux};       // widen lux to 32 bits so the shift below can't overflow
    wire [31:0] delta         = lux_ext << 12;      // "how much to subtract from PERIOD_MAX," before clamping
    wire [24:0] delta_clamped = (delta > DELTA_RANGE) ? DELTA_RANGE : delta[24:0]; // never subtract more than the whole range
    wire [24:0] period        = PERIOD_MAX - delta_clamped; // the actual half-period we'll use, this instant

    reg [24:0] blink_cnt = 0;
    reg        led_reg   = 0;

    always @(posedge CLK) begin
        // Simple free-running counter: count up every cycle, and flip the
        // LED (plus reset the counter) once we reach the current target
        // `period`. Because `period` is recomputed live from `lux` on
        // every single cycle (it's a `wire`, not a register that's only
        // updated occasionally), the blink rate smoothly tracks whatever
        // the most recent sensor reading was, without needing any extra
        // logic to "apply" a new reading.
        if (blink_cnt >= period) begin
            blink_cnt <= 0;
            led_reg   <= ~led_reg;
        end else begin
            blink_cnt <= blink_cnt + 1'b1;
        end
    end

    // Currently overridden below with a simple threshold indicator
    // instead of blink-rate -- see DIAG_LUX_THRESHOLD.
    // assign LED = led_reg; // normal blink-rate output: faster blink with more light

    // DIAGNOSTIC: LED on iff live lux (raw ALS_DATA count, updated every
    // ~500ms) is above threshold -- a simple, easy-to-read on/off signal
    // for confirming the sensor tracks real light levels.
    localparam [15:0] DIAG_LUX_THRESHOLD = 16'd1000;
    assign LED = (lux > DIAG_LUX_THRESHOLD);

endmodule
