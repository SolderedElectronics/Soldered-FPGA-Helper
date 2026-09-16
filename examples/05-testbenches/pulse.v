module pulse #(
    parameter COUNTER_BITS = 24  // real hardware: bit 23 flips every ~0.5s at 16MHz
) (
    input  wire clk,
    output reg  led
);

    reg [COUNTER_BITS-1:0] counter = 0;

    always @(posedge clk) begin
        counter <= counter + 1'b1;
        led     <= counter[COUNTER_BITS-1];
    end

endmodule
