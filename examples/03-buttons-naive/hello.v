module hello (
    input  BTN,     // active-low button, external pull-up
    output LED      // onboard USR LED
);

    assign LED = ~BTN;  // LED on when button pressed (BTN=0)

endmodule
