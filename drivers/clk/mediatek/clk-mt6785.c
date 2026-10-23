// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2025 Dinolek <git@dinolek.me>

#include <dm.h>
#include <dt-bindings/clock/mediatek,mt6785-clk.h>

#include "clk-mtk.h"

static const ulong ext_clock_rates[] = { 26000000 };

static const struct mtk_fixed_clk top_fixed_clks[] = {
	FIXED_CLK(CLK_TOP_ULPOSC, -1, 0, 260000000),
};

static const struct mtk_fixed_factor top_fixed_divs[] = {
	FACTOR(CLK_TOP_OSC_D2, CLK_TOP_ULPOSC, 1, 2, 0),
	FACTOR(CLK_TOP_OSC_D4, CLK_TOP_ULPOSC, 1, 4, 0),
	FACTOR(CLK_TOP_OSC_D8, CLK_TOP_ULPOSC, 1, 8, 0),
	FACTOR(CLK_TOP_OSC_D10, CLK_TOP_ULPOSC, 1, 10, 0),
	FACTOR(CLK_TOP_OSC_D16, CLK_TOP_ULPOSC, 1, 16, 0),
};

static const struct mtk_parent pwrap_parents[] = {
	TOP_PARENT(CLK_TOP_OSC_D10),
	EXT_PARENT(0),
	TOP_PARENT(CLK_TOP_OSC_D4),
	TOP_PARENT(CLK_TOP_OSC_D8),
	TOP_PARENT(CLK_TOP_OSC_D16),
};

static const struct mtk_composite top_muxes[] = {
	MUX_CLR_SET_UPD_FLAGS(CLK_TOP_PWRAP_ULPOSC, pwrap_parents,
			      0x80, 0x84, 0x88, 24, 3, 31, 0x4, 27,
			      CLK_MUX_SETCLR_UPD),
};

static const struct mtk_clk_tree topckgen_tree = {
	.ext_clk_rates = ext_clock_rates,
	.num_ext_clks = ARRAY_SIZE(ext_clock_rates),
	.fclks = top_fixed_clks,
	.num_fclks = ARRAY_SIZE(top_fixed_clks),
	.fdivs_offs = CLK_TOP_OSC_D2,
	.fdivs = top_fixed_divs,
	.num_fdivs = ARRAY_SIZE(top_fixed_divs),
	.muxes_offs = CLK_TOP_PWRAP_ULPOSC,
	.muxes = top_muxes,
	.num_muxes = ARRAY_SIZE(top_muxes),
	.flags = CLK_PARENT_TOPCKGEN,
	.type = MTK_CLK_TREE_TOPCKGEN,
};

static const struct mtk_gate_regs infra0_cg_regs = {
	.sta_ofs = 0x90,
	.clr_ofs = 0x84,
	.set_ofs = 0x80,
};

static const struct mtk_gate infra_clks[] = {
	GATE_FLAGS(CLK_INFRA_PMIC_TMR, CLK_TOP_PWRAP_ULPOSC, &infra0_cg_regs,
		   0, CLK_GATE_SETCLR | CLK_PARENT_TOPCKGEN),
	GATE_FLAGS(CLK_INFRA_PMIC_AP, CLK_TOP_PWRAP_ULPOSC, &infra0_cg_regs,
		   1, CLK_GATE_SETCLR | CLK_PARENT_TOPCKGEN),
};

static const struct mtk_clk_tree infracfg_tree = {
	.gates = infra_clks,
	.num_gates = ARRAY_SIZE(infra_clks),
	.flags = CLK_PARENT_INFRASYS,
	.type = MTK_CLK_TREE_INFRASYS,
};

static const struct udevice_id mt6785_clk_ids[] = {
	{ .compatible = "mediatek,mt6785-topckgen", .data = (ulong)&topckgen_tree },
	{ .compatible = "mediatek,mt6785-infracfg", .data = (ulong)&infracfg_tree },
	{ }
};

U_BOOT_DRIVER(mt6785_clk) = {
	.name = "mt6785-clk",
	.id = UCLASS_CLK,
	.of_match = mt6785_clk_ids,
	.probe = mtk_clk_probe,
	.priv_auto = sizeof(struct mtk_clk_priv),
	.ops = &mtk_clk_ops,
	.flags = DM_FLAG_PRE_RELOC,
};
