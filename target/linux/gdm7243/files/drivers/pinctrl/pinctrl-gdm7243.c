// SPDX-License-Identifier: GPL-2.0-only
/*
 * GCT GDM7243QT application CPU pin multiplexing.
 *
 * Clean-room mapping of the real Polish P4 kernel's 63 group descriptors.
 * Shared mux fields are arbitrated across groups, not guessed per pin.
 */
#include <linux/err.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/pinctrl/pinconf-generic.h>
#include <linux/pinctrl/pinctrl.h>
#include <linux/pinctrl/pinmux.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/string_helpers.h>

#define GDM7243_PIN_BANK_WIDTH 32
#define GDM7243_PIN_BANKS 7
#define GDM7243_PIN_COUNT (GDM7243_PIN_BANK_WIDTH * GDM7243_PIN_BANKS)

struct gdm7243_group_spec {
	const char *name;
	u16 first_pin;
	u16 last_pin;
	u16 first_option;
	u8 num_options;
};

struct gdm7243_mux_option {
	const char *function;
	u32 mask;
	u32 value;
};

#include "pinctrl-gdm7243-data.h"

struct gdm7243_group_state {
	unsigned int pins[GDM7243_PIN_BANK_WIDTH];
	unsigned int mux_refs;
	unsigned int gpio_refs;
	int active_option;
};

struct gdm7243_function {
	const char *name;
	const char **groups;
	unsigned int num_groups;
};

struct gdm7243_pinctrl {
	struct device *dev;
	void __iomem *base;
	struct mutex mutex;
	struct pinctrl_dev *pctl;
	struct pinctrl_desc desc;
	struct pinctrl_pin_desc *pins;
	struct gdm7243_group_state *state;
	struct gdm7243_function *functions;
	unsigned int function_count;
	int pin_groups[GDM7243_PIN_COUNT];
};

static int gdm7243_get_groups_count(struct pinctrl_dev *pctldev)
{
	return ARRAY_SIZE(gdm7243_group_specs);
}

static const char *gdm7243_get_group_name(struct pinctrl_dev *pctldev,
					 unsigned int selector)
{
	if (selector >= ARRAY_SIZE(gdm7243_group_specs))
		return NULL;
	return gdm7243_group_specs[selector].name;
}

static int gdm7243_get_group_pins(struct pinctrl_dev *pctldev,
				  unsigned int selector,
				  const unsigned int **pins,
				  unsigned int *num_pins)
{
	struct gdm7243_pinctrl *ctrl = pinctrl_dev_get_drvdata(pctldev);
	const struct gdm7243_group_spec *g;

	if (selector >= ARRAY_SIZE(gdm7243_group_specs))
		return -EINVAL;

	g = &gdm7243_group_specs[selector];
	*pins = ctrl->state[selector].pins;
	*num_pins = g->last_pin - g->first_pin + 1;
	return 0;
}

static const struct pinctrl_ops gdm7243_pctl_ops = {
	.get_groups_count = gdm7243_get_groups_count,
	.get_group_name = gdm7243_get_group_name,
	.get_group_pins = gdm7243_get_group_pins,
	.dt_node_to_map = pinconf_generic_dt_node_to_map_group,
	.dt_free_map = pinconf_generic_dt_free_map,
};

static int gdm7243_get_functions_count(struct pinctrl_dev *pctldev)
{
	struct gdm7243_pinctrl *ctrl = pinctrl_dev_get_drvdata(pctldev);

	return ctrl->function_count;
}

static const char *gdm7243_get_function_name(struct pinctrl_dev *pctldev,
					     unsigned int selector)
{
	struct gdm7243_pinctrl *ctrl = pinctrl_dev_get_drvdata(pctldev);

	if (selector >= ctrl->function_count)
		return NULL;
	return ctrl->functions[selector].name;
}

static int gdm7243_get_function_groups(struct pinctrl_dev *pctldev,
					unsigned int selector,
					const char * const **groups,
					unsigned int *num_groups)
{
	struct gdm7243_pinctrl *ctrl = pinctrl_dev_get_drvdata(pctldev);

	if (selector >= ctrl->function_count)
		return -EINVAL;

	*groups = ctrl->functions[selector].groups;
	*num_groups = ctrl->functions[selector].num_groups;
	return 0;
}

static int gdm7243_group_option(unsigned int group, const char *function)
{
	const struct gdm7243_group_spec *spec = &gdm7243_group_specs[group];
	unsigned int i;

	for (i = spec->first_option;
	     i < spec->first_option + spec->num_options; i++) {
		if (!strcmp(gdm7243_options[i].function, function))
			return i;
	}
	return -EINVAL;
}

/*
 * All seven banks have a single 32-bit mux selector word. Most groups own
 * disjoint fields, but GPE groups 0-10 and 12-15 share selector bits [2:0].
 * They may coexist only if both request the same encoding. A group retains
 * its claim until the pinmux core releases the last pin in that group.
 */
static int gdm7243_apply_group(struct gdm7243_pinctrl *ctrl,
			       unsigned int group, int option)
{
	const struct gdm7243_group_spec *spec = &gdm7243_group_specs[group];
	const struct gdm7243_mux_option *new = &gdm7243_options[option];
	unsigned int bank = spec->first_pin / GDM7243_PIN_BANK_WIDTH;
	unsigned int i;
	void __iomem *reg = ctrl->base + bank * sizeof(u32);
	u32 value;

	for (i = 0; i < ARRAY_SIZE(gdm7243_group_specs); i++) {
		const struct gdm7243_group_spec *other = &gdm7243_group_specs[i];
		struct gdm7243_group_state *state = &ctrl->state[i];
		const struct gdm7243_mux_option *old;
		u32 overlap;

		if (i == group || (!state->mux_refs && !state->gpio_refs) ||
		    state->active_option < 0 ||
		    other->first_pin / GDM7243_PIN_BANK_WIDTH != bank)
			continue;

		old = &gdm7243_options[state->active_option];
		overlap = new->mask & old->mask;
		if (((new->value ^ old->value) & overlap) != 0)
			return -EBUSY;
	}

	if (new->mask) {
		value = readl(reg);
		writel((value & ~new->mask) | new->value, reg);
	}
	ctrl->state[group].active_option = option;
	return 0;
}

static int gdm7243_set_mux(struct pinctrl_dev *pctldev,
			    unsigned int func_selector,
			    unsigned int group_selector)
{
	struct gdm7243_pinctrl *ctrl = pinctrl_dev_get_drvdata(pctldev);
	struct gdm7243_group_state *state;
	int option, ret;

	if (func_selector >= ctrl->function_count ||
	    group_selector >= ARRAY_SIZE(gdm7243_group_specs))
		return -EINVAL;

	option = gdm7243_group_option(group_selector,
					ctrl->functions[func_selector].name);
	if (option < 0)
		return option;

	state = &ctrl->state[group_selector];
	mutex_lock(&ctrl->mutex);
	if (state->gpio_refs)
		ret = -EBUSY;
	else
		ret = gdm7243_apply_group(ctrl, group_selector, option);
	mutex_unlock(&ctrl->mutex);
	return ret;
}

static int gdm7243_request_pin(struct pinctrl_dev *pctldev,
				unsigned int pin)
{
	struct gdm7243_pinctrl *ctrl = pinctrl_dev_get_drvdata(pctldev);
	struct gdm7243_group_state *state;
	int group, ret = 0;

	if (pin >= GDM7243_PIN_COUNT)
		return -EINVAL;
	group = ctrl->pin_groups[pin];
	if (group < 0)
		return -ENOTSUPP;
	state = &ctrl->state[group];

	mutex_lock(&ctrl->mutex);
	if (state->gpio_refs)
		ret = -EBUSY;
	else
		state->mux_refs++;
	mutex_unlock(&ctrl->mutex);
	return ret;
}

static int gdm7243_free_pin(struct pinctrl_dev *pctldev,
			     unsigned int pin)
{
	struct gdm7243_pinctrl *ctrl = pinctrl_dev_get_drvdata(pctldev);
	struct gdm7243_group_state *state;

	if (pin >= GDM7243_PIN_COUNT || ctrl->pin_groups[pin] < 0)
		return -EINVAL;

	state = &ctrl->state[ctrl->pin_groups[pin]];
	mutex_lock(&ctrl->mutex);
	if (WARN_ON(!state->mux_refs)) {
		mutex_unlock(&ctrl->mutex);
		return -EINVAL;
	}
	if (--state->mux_refs == 0 && !state->gpio_refs)
		state->active_option = -1;
	mutex_unlock(&ctrl->mutex);
	return 0;
}

static int gdm7243_gpio_request_enable(struct pinctrl_dev *pctldev,
					struct pinctrl_gpio_range *range,
					unsigned int pin)
{
	struct gdm7243_pinctrl *ctrl = pinctrl_dev_get_drvdata(pctldev);
	struct gdm7243_group_state *state;
	int group, option, ret;

	if (pin >= GDM7243_PIN_COUNT)
		return -EINVAL;
	group = ctrl->pin_groups[pin];
	if (group < 0)
		return -ENOTSUPP;

	state = &ctrl->state[group];
	option = gdm7243_group_option(group, "gpio");
	if (option < 0)
		return option;

	mutex_lock(&ctrl->mutex);
	if (state->mux_refs)
		ret = -EBUSY;
	else
		ret = gdm7243_apply_group(ctrl, group, option);
	if (!ret)
		state->gpio_refs++;
	mutex_unlock(&ctrl->mutex);
	return ret;
}

static void gdm7243_gpio_disable_free(struct pinctrl_dev *pctldev,
				       struct pinctrl_gpio_range *range,
				       unsigned int pin)
{
	struct gdm7243_pinctrl *ctrl = pinctrl_dev_get_drvdata(pctldev);
	struct gdm7243_group_state *state;

	if (pin >= GDM7243_PIN_COUNT || ctrl->pin_groups[pin] < 0)
		return;
	state = &ctrl->state[ctrl->pin_groups[pin]];
	mutex_lock(&ctrl->mutex);
	if (WARN_ON(!state->gpio_refs)) {
		mutex_unlock(&ctrl->mutex);
		return;
	}
	if (--state->gpio_refs == 0 && !state->mux_refs)
		state->active_option = -1;
	mutex_unlock(&ctrl->mutex);
}

static const struct pinmux_ops gdm7243_pmx_ops = {
	.get_functions_count = gdm7243_get_functions_count,
	.get_function_name = gdm7243_get_function_name,
	.get_function_groups = gdm7243_get_function_groups,
	.set_mux = gdm7243_set_mux,
	.request = gdm7243_request_pin,
	.free = gdm7243_free_pin,
	.gpio_request_enable = gdm7243_gpio_request_enable,
	.gpio_disable_free = gdm7243_gpio_disable_free,
	.strict = true,
};

static int gdm7243_init_groups(struct gdm7243_pinctrl *ctrl)
{
	unsigned int i, pin, opt;

	for (pin = 0; pin < GDM7243_PIN_COUNT; pin++)
		ctrl->pin_groups[pin] = -1;

	for (i = 0; i < ARRAY_SIZE(gdm7243_group_specs); i++) {
		const struct gdm7243_group_spec *spec = &gdm7243_group_specs[i];
		struct gdm7243_group_state *state = &ctrl->state[i];

		if (spec->first_pin > spec->last_pin ||
		    spec->last_pin >= GDM7243_PIN_COUNT ||
		    spec->first_pin / GDM7243_PIN_BANK_WIDTH !=
		    spec->last_pin / GDM7243_PIN_BANK_WIDTH ||
		    !spec->num_options ||
		    spec->first_option + spec->num_options >
		    ARRAY_SIZE(gdm7243_options))
			return -EINVAL;

		for (opt = spec->first_option;
		     opt < spec->first_option + spec->num_options; opt++)
			if (gdm7243_options[opt].value &
			    ~gdm7243_options[opt].mask)
				return -EINVAL;

		state->active_option = -1;
		for (pin = spec->first_pin; pin <= spec->last_pin; pin++) {
			if (ctrl->pin_groups[pin] != -1)
				return -EINVAL;
			ctrl->pin_groups[pin] = i;
			state->pins[pin - spec->first_pin] = pin;
		}
	}
	return 0;
}

static int gdm7243_init_functions(struct gdm7243_pinctrl *ctrl)
{
	struct device *dev = ctrl->dev;
	unsigned int i, j, group;

	ctrl->functions = devm_kcalloc(dev, ARRAY_SIZE(gdm7243_options),
				       sizeof(*ctrl->functions), GFP_KERNEL);
	if (!ctrl->functions)
		return -ENOMEM;

	for (i = 0; i < ARRAY_SIZE(gdm7243_options); i++) {
		const char *name = gdm7243_options[i].function;

		for (j = 0; j < ctrl->function_count; j++)
			if (!strcmp(name, ctrl->functions[j].name))
				break;
		if (j == ctrl->function_count) {
			ctrl->functions[j].name = name;
			ctrl->function_count++;
		}
		ctrl->functions[j].num_groups++;
	}

	for (i = 0; i < ctrl->function_count; i++) {
		struct gdm7243_function *f = &ctrl->functions[i];
		unsigned int count = f->num_groups;

		f->groups = devm_kcalloc(dev, count, sizeof(*f->groups),
					  GFP_KERNEL);
		if (!f->groups)
			return -ENOMEM;
		f->num_groups = 0;
	}

	for (group = 0; group < ARRAY_SIZE(gdm7243_group_specs); group++) {
		const struct gdm7243_group_spec *spec = &gdm7243_group_specs[group];

		for (i = spec->first_option;
		     i < spec->first_option + spec->num_options; i++) {
			const char *name = gdm7243_options[i].function;

			for (j = 0; j < ctrl->function_count; j++) {
				struct gdm7243_function *f = &ctrl->functions[j];

				if (!strcmp(f->name, name)) {
					f->groups[f->num_groups++] = spec->name;
					break;
				}
			}
		}
	}
	return 0;
}

static int gdm7243_pinctrl_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct gdm7243_pinctrl *ctrl;
	char **pin_names;
	unsigned int i;
	int ret;

	ctrl = devm_kzalloc(dev, sizeof(*ctrl), GFP_KERNEL);
	if (!ctrl)
		return -ENOMEM;
	ctrl->dev = dev;
	mutex_init(&ctrl->mutex);
	ctrl->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(ctrl->base))
		return PTR_ERR(ctrl->base);

	ctrl->state = devm_kcalloc(dev, ARRAY_SIZE(gdm7243_group_specs),
				   sizeof(*ctrl->state), GFP_KERNEL);
	ctrl->pins = devm_kcalloc(dev, GDM7243_PIN_COUNT,
				  sizeof(*ctrl->pins), GFP_KERNEL);
	if (!ctrl->state || !ctrl->pins)
		return -ENOMEM;

	ret = gdm7243_init_groups(ctrl);
	if (ret)
		return dev_err_probe(dev, ret, "invalid pinmux group table\n");
	ret = gdm7243_init_functions(ctrl);
	if (ret)
		return ret;

	pin_names = devm_kasprintf_strarray(dev, "gpio", GDM7243_PIN_COUNT);
	if (IS_ERR(pin_names))
		return PTR_ERR(pin_names);
	for (i = 0; i < GDM7243_PIN_COUNT; i++) {
		ctrl->pins[i].number = i;
		ctrl->pins[i].name = pin_names[i];
	}

	ctrl->desc.name = "gdm7243-pinctrl";
	ctrl->desc.owner = THIS_MODULE;
	ctrl->desc.pins = ctrl->pins;
	ctrl->desc.npins = GDM7243_PIN_COUNT;
	ctrl->desc.pctlops = &gdm7243_pctl_ops;
	ctrl->desc.pmxops = &gdm7243_pmx_ops;

	ctrl->pctl = devm_pinctrl_register(dev, &ctrl->desc, ctrl);
	if (IS_ERR(ctrl->pctl))
		return dev_err_probe(dev, PTR_ERR(ctrl->pctl),
				     "failed to register pinctrl\n");

	platform_set_drvdata(pdev, ctrl);
	dev_info(dev, "registered %zu groups and %u functions\n",
		 ARRAY_SIZE(gdm7243_group_specs), ctrl->function_count);
	return 0;
}

static const struct of_device_id gdm7243_pinctrl_of_match[] = {
	{ .compatible = "gct,gdm7243-pinctrl" },
	{ }
};
MODULE_DEVICE_TABLE(of, gdm7243_pinctrl_of_match);

static struct platform_driver gdm7243_pinctrl_driver = {
	.probe = gdm7243_pinctrl_probe,
	.driver = {
		.name = "gdm7243-pinctrl",
		.of_match_table = gdm7243_pinctrl_of_match,
	},
};
module_platform_driver(gdm7243_pinctrl_driver);

MODULE_DESCRIPTION("GCT GDM7243QT application CPU pinctrl driver");
MODULE_LICENSE("GPL");
