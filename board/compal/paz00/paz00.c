/*
 * Copyright (c) 2010-2012, NVIDIA CORPORATION.  All rights reserved.
 *
 * See file CREDITS for list of people who contributed to this
 * project.
 *
 * SPDX-License-Identifier:	GPL-2.0
 */

#include <common.h>
#include <asm/io.h>
#include <asm/arch/clock.h>
#include <asm/arch/funcmux.h>
#include <asm/arch/pinmux.h>
#include <asm/arch/tegra.h>
#include <asm/gpio.h>

#ifdef CONFIG_TEGRA_MMC
/*
 * Routine: pin_mux_mmc
 * Description: setup the pin muxes/tristate values for the SDMMC(s)
 */
void pin_mux_mmc(void)
{
	/* SDMMC4: config 3, x8 on 2nd set of pins */
	pinmux_set_func(PMUX_PINGRP_ATB, PMUX_FUNC_SDIO4);
	pinmux_set_func(PMUX_PINGRP_GMA, PMUX_FUNC_SDIO4);
	pinmux_set_func(PMUX_PINGRP_GME, PMUX_FUNC_SDIO4);

	pinmux_tristate_disable(PMUX_PINGRP_ATB);
	pinmux_tristate_disable(PMUX_PINGRP_GMA);
	pinmux_tristate_disable(PMUX_PINGRP_GME);

	/* SDIO1: SDIO1_CLK, SDIO1_CMD, SDIO1_DAT[3:0] */
	pinmux_set_func(PMUX_PINGRP_SDIO1, PMUX_FUNC_SDIO1);

	pinmux_tristate_disable(PMUX_PINGRP_SDIO1);

	/* For power GPIO PV1 */
	pinmux_tristate_disable(PMUX_PINGRP_UAC);
	/* For CD GPIO PV5 */
	pinmux_tristate_disable(PMUX_PINGRP_GPV);
}
#endif

#ifdef CONFIG_USB_EHCI_TEGRA
void pin_mux_usb(void)
{
	/* For USB1's ULPI signals */
	funcmux_select(PERIPH_ID_USB2, FUNCMUX_USB2_ULPI);

	/* ULPI reference clock output */
	pinmux_set_func(PMUX_PINGRP_CDEV2, PMUX_FUNC_PLLP_OUT4);
	pinmux_tristate_disable(PMUX_PINGRP_CDEV2);

	/* USB1 PHY reset GPIO */
	gpio_request(TEGRA_GPIO(V, 0), "ulpi_phy_reset");
	gpio_direction_output(TEGRA_GPIO(V, 0), 0);
	udelay(5);
	gpio_set_value(TEGRA_GPIO(V, 0), 1);
}
#endif

#ifdef CONFIG_DM_VIDEO
/* this is a weak define that we are overriding */
void pin_mux_display(void)
{
	debug("init display pinmux\n");

	/* EN_VDD_PANEL GPIO A4 */
	pinmux_tristate_disable(PMUX_PINGRP_DAP2);
}
#endif
