module hello (
    input  CLK,     // 16MHz onboard oscillator
    input  BTN,     // active-low button, external pull-up
    output LED      // onboard USR LED
);

    localparam DEBOUNCE_CYCLES = 160_000; // ~10ms at 16MHz
    localparam CNT_WIDTH       = 18;      // 2^18 > DEBOUNCE_CYCLES

    reg [1:0]            btn_sync   = 2'b11; // 2-FF synchronizer, idle-high
    reg                  btn_stable = 1'b1;
    reg [CNT_WIDTH-1:0]  cnt        = 0;

    always @(posedge CLK) begin
        btn_sync <= {btn_sync[0], BTN};

        if (btn_sync[1] == btn_stable) begin
            cnt <= 0;
        end else begin
            cnt <= cnt + 1'b1;
            if (cnt >= DEBOUNCE_CYCLES) begin
                btn_stable <= btn_sync[1];
                cnt        <= 0;
            end
        end
    end

    assign LED = ~btn_stable; // LED on when debounced button is pressed

endmodule
