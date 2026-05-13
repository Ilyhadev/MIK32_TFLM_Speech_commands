#include "riscv-irq.h"
#include "csr.h"
#include "scr1_csr_encoding.h"

irqfunc_t *riscv_irq_handler_map[RISCV_IRQ_NUMS];
irqfunc_t *riscv_exception_handler_map[RISCV_EXCP_NUMS];

void riscv_irq_enable(unsigned int irq_num) { set_csr(mie, 1 << irq_num); }

void riscv_irq_disable(unsigned int irq_num) { clear_csr(mie, 1 << irq_num); }

void riscv_irq_set_handler(unsigned int irq_num, irqfunc_t *handler) {
  riscv_irq_handler_map[irq_num] = handler;
}

void riscv_exception_set_handler(unsigned int exception_num,
                                 irqfunc_t *handler) {
  riscv_exception_handler_map[exception_num] = handler;
}

void riscv_irq_global_enable(void) { set_csr(mstatus, MSTATUS_MIE); }

void riscv_irq_global_disable(void) { clear_csr(mstatus, MSTATUS_MIE); }

void _default_exception_handler(uint16_t except_num) {

  while (1) {
  };
}

void trap_handler(void) {
  uint32_t mcause = read_csr(mcause);

  if ((mcause & MCAUSE_INT) == 0) {
    // handle exception
    mcause &= 0xFF;
    if (mcause >= RISCV_EXCP_NUMS) {
      while (1) {
      } // error
    }
    if (riscv_exception_handler_map[mcause] != 0) {
      riscv_exception_handler_map[mcause]();
    } else {
      _default_exception_handler(mcause); // NO handler,
    }
    // offset mepc to next instruction, considering the length of the
    // instruction.
    uint32_t mepc = read_csr(mepc);
    mepc = (*((uint8_t *)(mepc)) & 0x3) == 0x3 ? mepc + 4 : mepc + 2;
    write_csr(mepc, mepc);
  } else {
    // handle interrupt
    mcause &= 0xFF;
    if (mcause >= RISCV_IRQ_NUMS) {
      while (1) {
      } // error
    }
    if (riscv_irq_handler_map[mcause] != 0) {
      riscv_irq_handler_map[mcause]();
    } else {
      while (1) {
      }; // NO handler
    }
  }
}