`timescale 1ns/1ps

module pulse_tb;

    reg CLK = 0;
    wire LED;

    always #31.25 CLK = ~CLK;

    pulse #(
        .COUNTER_BITS(4)
    ) dut (
        .clk (CLK),
        .led (LED)
    );

    initial $dumpvars(0, pulse_tb);

    integer toggle_count = 0;
    always @(LED) toggle_count = toggle_count + 1;

    initial begin
        #4000;
        if (toggle_count < 4) begin
            $display("FAIL: expected at least 4 LED toggles in 4000ns, got %0d", toggle_count);
            if (!`APIO_SIM) $fatal;
        end else begin
            $display("PASS: %0d LED toggles observed", toggle_count);
        end
        $finish;
    end

endmodule
