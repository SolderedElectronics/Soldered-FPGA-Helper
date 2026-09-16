`include "defs.vh"

module blink (
    input  wire clk,
    output reg  led
);

    reg [23:0] counter = 24'd0;

    always @(posedge clk) begin
        counter <= counter + 1'b1;
        led     <= counter[`BLINK_BIT];
    end

endmodule
