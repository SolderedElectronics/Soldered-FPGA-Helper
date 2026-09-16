//------------------------------------------------------------------
//-- Hash race: N parallel lanes searching for a hash match.
//-- Hash is a Murmur3-style avalanche finalizer (xor-shift + odd-constant
//-- multiply, twice) -- nonlinear, so it's still a full 32-bit bijection
//-- (exactly one candidate matches TARGET, guaranteed run length, computed
//-- backward in Python) but NOT representable as a compiler-solvable affine
//-- relation. That matters for the ESP32 side: a plain multiply hash let
//-- GCC solve the whole search symbolically at compile time instead of
//-- actually running it. This mix defeats that while staying real, optimized
//-- code on both sides -- same math here and in hash_race_esp32.ino.
//------------------------------------------------------------------

module hash_race #(
    parameter N            = 4,          // parallel lanes
    parameter [31:0] TARGET = 32'h0441D1D5 // = fmix32(410_000_000): guarantees match lands exactly there
)(
    input  CLK,   // 16MHz onboard clock
    input  BTN,   // PIN_19 (ball B8): shared start button, external pullup to 3V3, pressed = 0
    output LED,   // lit once any lane finds a match
    output USBPU  // USB pull-up, disabled
);

    assign USBPU = 0;

    // 2-FF synchronizer for the async external button, then latch on first
    // press so the race starts exactly once and keeps running if released.
    reg btn_sync0, btn_sync1;
    reg run;

    initial begin
        btn_sync0 = 1'b1;
        btn_sync1 = 1'b1;
        run       = 1'b0;
    end

    always @(posedge CLK) begin
        btn_sync0 <= BTN;
        btn_sync1 <= btn_sync0;
        if (!run && !btn_sync1) run <= 1'b1;
    end

    function automatic [31:0] fmix32;
        input [31:0] h_in;
        reg   [31:0] h;
        begin
            h = h_in;
            h = h ^ (h >> 16);
            h = h * 32'h85EBCA6B;
            h = h ^ (h >> 13);
            h = h * 32'hC2B2AE35;
            h = h ^ (h >> 16);
            fmix32 = h;
        end
    endfunction

    reg [31:0] candidate [0:N-1];
    reg        found_lane [0:N-1];

    genvar i;
    generate
        for (i = 0; i < N; i = i + 1) begin : lanes
            reg [31:0] hash;

            initial begin
                candidate[i]  = i;   // offset each lane so they don't overlap
                found_lane[i] = 1'b0;
                hash          = 32'd0;
            end

            always @(posedge CLK) begin
                if (run && !found_lane[i]) begin
                    hash <= fmix32(candidate[i]);
                    if (hash == TARGET)
                        found_lane[i] <= 1'b1;
                    candidate[i] <= candidate[i] + N;
                end
            end
        end
    endgenerate

    integer j;
    reg found;
    always @(*) begin
        found = 1'b0;
        for (j = 0; j < N; j = j + 1) found = found | found_lane[j];
    end

    assign LED = found;

endmodule
