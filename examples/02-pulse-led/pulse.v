module pulse (
    input  wire clk,
    output reg  led
);

    reg [23:0] counter = 24'd0;

    always @(posedge clk) begin
        counter <= counter + 1'b1;
        led     <= counter[23];
    end

endmodule
