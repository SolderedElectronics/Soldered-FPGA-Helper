`timescale 1ns/1ps

module tb_hello;

    reg CLK = 0;
    wire LED;
    wire SDA;
    wire SCL;

    always #31.25 CLK = ~CLK; // 16MHz

    pullup(SDA);
    pullup(SCL);

    hello #(
        .QUARTER(40),
        .STARTUP_CYCLES(500),
        .LOOP_CYCLES(500)
    ) dut (
        .CLK(CLK),
        .LED(LED),
        .SDA(SDA),
        .SCL(SCL)
    );

    // -----------------------------------------------------------------
    // Minimal I2C slave model: ACKs address 0x3A, ACKs any register
    // write, and on a read returns a fixed 2-byte lux value so we can
    // check the master captured it correctly.
    // -----------------------------------------------------------------
    localparam [7:0] FAKE_LUX_LOW  = 8'h34;
    localparam [7:0] FAKE_LUX_HIGH = 8'h12; // expect captured lux == 16'h1234

    reg        s_drive = 0;
    reg        s_val   = 0;
    assign SDA = s_drive ? (s_val ? 1'bz : 1'b0) : 1'bz;

    reg [7:0] s_shift;
    reg [3:0] s_bitcnt;
    reg [7:0] s_last_byte;
    reg       s_is_write_addr;
    reg [7:0] s_bytes_seen [0:15];
    integer   s_byte_count = 0;

    task s_release_sda; begin s_drive <= 0; end endtask
    task s_drive_bit(input b); begin s_drive <= 1; s_val <= b; end endtask

    initial begin
        s_drive = 0;
        s_byte_count = 0;
        forever begin
            // wait for START: SDA falls while SCL high
            @(negedge SDA);
            if (SCL === 1'b1) begin
                s_bitcnt = 0;
                s_shift  = 0;
                // shift in 8 address/data bits
                repeat (8) begin
                    @(posedge SCL);
                    s_shift = {s_shift[6:0], SDA};
                    @(negedge SCL);
                end
                // drive ACK -- already at start of the ack bit's low phase
                // (the repeat loop above just consumed that negedge)
                s_drive_bit(1'b0); // ACK
                @(posedge SCL);
                @(negedge SCL);
                s_release_sda;

                s_bytes_seen[s_byte_count] = s_shift;
                s_byte_count = s_byte_count + 1;
                $display("[%0t] SLAVE: got byte 0x%02h", $time, s_shift);

                // If this was the address+R (0x75), start sending fake data.
                // We're already at the start of byte0's bit7 low phase (the
                // ack-bit handling above just consumed that negedge), so the
                // first bit of each byte is driven immediately, not after an
                // extra @(negedge SCL) -- otherwise every bit ends up shifted.
                if (s_shift == 8'h75) begin
                    // byte 0: FAKE_LUX_LOW
                    s_drive_bit(FAKE_LUX_LOW[7]);
                    @(posedge SCL);
                    for (s_bitcnt = 1; s_bitcnt < 8; s_bitcnt = s_bitcnt + 1) begin
                        @(negedge SCL);
                        s_drive_bit(FAKE_LUX_LOW[7-s_bitcnt]);
                        @(posedge SCL);
                    end
                    @(negedge SCL);
                    s_release_sda; // let master drive ack/nack
                    @(posedge SCL);
                    $display("[%0t] SLAVE: master ack/nack after byte0 = %b", $time, SDA);
                    @(negedge SCL);

                    // byte 1: FAKE_LUX_HIGH
                    s_drive_bit(FAKE_LUX_HIGH[7]);
                    @(posedge SCL);
                    for (s_bitcnt = 1; s_bitcnt < 8; s_bitcnt = s_bitcnt + 1) begin
                        @(negedge SCL);
                        s_drive_bit(FAKE_LUX_HIGH[7-s_bitcnt]);
                        @(posedge SCL);
                    end
                    @(negedge SCL);
                    s_release_sda;
                    @(posedge SCL);
                    $display("[%0t] SLAVE: master ack/nack after byte1 = %b", $time, SDA);
                    @(negedge SCL);
                end
            end
        end
    end

    // -----------------------------------------------------------------
    // Check the captured lux register inside the DUT
    // -----------------------------------------------------------------
    initial begin
        $dumpvars(0, tb_hello);

        // run long enough to cover startup wait + init txn + loop wait + one read
        #1_200_000; // 1.2ms of simulated time

        if (dut.lux == {FAKE_LUX_HIGH, FAKE_LUX_LOW}) begin
            $display("PASS: captured lux = 0x%04h", dut.lux);
        end else begin
            $display("FAIL: captured lux = 0x%04h, expected 0x%04h", dut.lux, {FAKE_LUX_HIGH, FAKE_LUX_LOW});
        end
        $finish;
    end

    initial begin
        #1_500_000;
        $display("TIMEOUT: simulation did not finish in time");
        $finish;
    end

endmodule
