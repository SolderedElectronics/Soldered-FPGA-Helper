`ifndef I2C_MASTER_VH
`define I2C_MASTER_VH

// This is a Verilog "header" file. It isn't compiled on its own -- other
// files pull it in with `include "i2c_master.vh"`, the same idea as a
// #include in C. Its only job here is to hold constants that need to match
// EXACTLY between i2c_master.v (the engine) and whatever module drives it
// (hello.v). Putting them in one shared place means both sides can never
// disagree about what "2'd1" is supposed to mean.
//
// These three values are the "command" you hand to the i2c_master engine's
// `cmd` input to tell it what to do next:
//   `I2C_CMD_START -- send an I2C START (or repeated-START) condition
//   `I2C_CMD_BYTE  -- send or receive one 8-bit byte (plus its ack bit)
//   `I2C_CMD_STOP  -- send an I2C STOP condition
// `define here works like a find-and-replace macro, not a typed constant --
// anywhere `I2C_CMD_START appears in code, the Verilog preprocessor swaps in
// 2'd0 before compilation even starts.
`define I2C_CMD_START 2'd0
`define I2C_CMD_BYTE  2'd1
`define I2C_CMD_STOP  2'd2

`endif
