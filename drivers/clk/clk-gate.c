
#include <linux/clk.h>
#include <linux/module.h>
#include <asm/io.h>

#define to_clk_gate(clk) container_of(clk, struct clk_gate, hw)

static unsigned long clk_gate_get_rate(struct clk_hw *clk)
{
	return clk_get_rate(clk_get_parent(clk->clk));
}

static int clk_gate_enable(struct clk_hw *clk)
{
	struct clk_gate *gate = to_clk_gate(clk);
	u32 reg;

	reg = __raw_readl(gate->reg);
	reg |= 1 << gate->bit_idx;
	__raw_writel(reg, gate->reg);

	return 0;
}

static void clk_gate_disable(struct clk_hw *clk)
{
	struct clk_gate *gate = to_clk_gate(clk);
	u32 reg;

	reg = __raw_readl(gate->reg);
	reg &= ~(1 << gate->bit_idx);
	__raw_writel(reg, gate->reg);
}

struct clk_hw_ops clk_gate_ops = {
	.recalc_rate = clk_gate_get_rate,
	.enable = clk_gate_enable,
	.disable = clk_gate_disable,
};
EXPORT_SYMBOL_GPL(clk_gate_ops);

