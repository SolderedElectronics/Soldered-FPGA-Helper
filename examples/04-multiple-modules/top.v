module top (
    input  wire CLK,
    output wire LED
);

    blink blink_inst (
        .clk (CLK),
        .led (LED)
    );

endmodule
