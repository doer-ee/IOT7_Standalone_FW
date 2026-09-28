#include "soc/gpio_reg.h"
#include "soc/io_mux_reg.h"
#include "soc/soc.h"

#define POWER_LATCH_GPIO 35
#define POWER_LATCH_BIT (POWER_LATCH_GPIO - 32)

/* Keep the external power latch enabled before bootloader initialization. */
void bootloader_before_init(void)
{
    PIN_FUNC_SELECT(IO_MUX_GPIO35_REG, FUNC_GPIO35_GPIO35);
    REG_SET_BIT(GPIO_OUT1_W1TS_REG, BIT(POWER_LATCH_BIT));
    REG_SET_BIT(GPIO_ENABLE1_W1TS_REG, BIT(POWER_LATCH_BIT));
}

/* Force this object file into the bootloader so the weak hook is overridden. */
void bootloader_hooks_include(void)
{
}
