
#include <linux/clk.h>
#include <linux/module.h>

#define to_clk_fixed(c) container_of(c, struct clk_hw_fixed, hw)

static unsigned long clk_fixed_recalc_rate(struct clk_hw *hw)
{
	return to_clk_fixed(hw)->rate;
}

struct clk_hw_ops clk_fixed_ops = {
	.recalc_rate = clk_fixed_recalc_rate,
};
EXPORT_SYMBOL_GPL(clk_fixed_ops);


