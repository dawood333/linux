// SPDX-License-Identifier: GPL-2.0
/* Willsemi WL2866D four-channel regulator driver - Xiaomi Ares port */
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/regmap.h>
#include <linux/regulator/driver.h>
#include <linux/regulator/of_regulator.h>
#include <linux/string.h>

#define WL2866D_ENABLE_REG 0x0e
#define WL2866D_VSET_BASE 0x03
#define WL2866D_DISCHARGE_REG 0x02
#define WL2866D_DVDD_BASE_UV 600000
#define WL2866D_DVDD_STEP_UV 6000
#define WL2866D_DVDD_MAX_UV 1800000
#define WL2866D_AVDD_BASE_UV 1200000
#define WL2866D_AVDD_STEP_UV 12500
#define WL2866D_AVDD_MAX_UV 4300000

static const char * const wl2866d_channel_names[] = {
	"wl2866d-dvdd1",
	"wl2866d-dvdd2",
	"wl2866d-avdd1",
	"wl2866d-avdd2",
};

struct wl2866d_chip { struct device *dev; struct regmap *map; };
struct wl2866d_ldo {
	struct wl2866d_chip *chip;
	struct regulator_desc desc;
	unsigned int offset;
	bool dvdd;
	int default_uv;
};

static const struct regmap_config wl2866d_regmap_config = {
	.reg_bits = 8, .val_bits = 8,
};

static int wl2866d_is_enabled(struct regulator_dev *rdev)
{
	struct wl2866d_ldo *ldo = rdev_get_drvdata(rdev);
	unsigned int val;
	int ret = regmap_read(ldo->chip->map, WL2866D_ENABLE_REG, &val);
	return ret ? ret : !!(val & BIT(ldo->offset));
}

static int wl2866d_enable(struct regulator_dev *rdev)
{
	struct wl2866d_ldo *ldo = rdev_get_drvdata(rdev);
	return regmap_update_bits(ldo->chip->map, WL2866D_ENABLE_REG,
				 BIT(ldo->offset), BIT(ldo->offset));
}

static int wl2866d_disable(struct regulator_dev *rdev)
{
	struct wl2866d_ldo *ldo = rdev_get_drvdata(rdev);
	return regmap_update_bits(ldo->chip->map, WL2866D_ENABLE_REG,
				 BIT(ldo->offset), 0);
}

static int wl2866d_get_voltage(struct regulator_dev *rdev)
{
	struct wl2866d_ldo *ldo = rdev_get_drvdata(rdev);
	unsigned int code;
	int ret = regmap_read(ldo->chip->map, WL2866D_VSET_BASE + ldo->offset, &code);
	if (ret)
		return ret;
	if (!code)
		return ldo->default_uv;
	return ldo->dvdd ? WL2866D_DVDD_BASE_UV + code * WL2866D_DVDD_STEP_UV :
		WL2866D_AVDD_BASE_UV + code * WL2866D_AVDD_STEP_UV;
}

static int wl2866d_set_voltage(struct regulator_dev *rdev, int min_uv, int max_uv,
			       unsigned int *selector)
{
	struct wl2866d_ldo *ldo = rdev_get_drvdata(rdev);
	unsigned int base = ldo->dvdd ? WL2866D_DVDD_BASE_UV : WL2866D_AVDD_BASE_UV;
	unsigned int step = ldo->dvdd ? WL2866D_DVDD_STEP_UV : WL2866D_AVDD_STEP_UV;
	unsigned int code, actual;
	unsigned int max_uv_supported = ldo->dvdd ? WL2866D_DVDD_MAX_UV :
		WL2866D_AVDD_MAX_UV;
	int ret;

	if (min_uv < base || max_uv < min_uv || max_uv > max_uv_supported)
		return -EINVAL;
	code = DIV_ROUND_UP(min_uv - base, step);
	actual = base + code * step;
	if (actual > max_uv || actual > max_uv_supported || code > U8_MAX)
		return -EINVAL;
	ret = regmap_write(ldo->chip->map, WL2866D_VSET_BASE + ldo->offset, code);
	if (!ret && selector)
		*selector = code;
	return ret;
}

static const struct regulator_ops wl2866d_ops = {
	.enable = wl2866d_enable, .disable = wl2866d_disable,
	.is_enabled = wl2866d_is_enabled, .get_voltage = wl2866d_get_voltage,
	.set_voltage = wl2866d_set_voltage,
};

static int wl2866d_program_init_voltage(struct wl2866d_ldo *ldo)
{
	unsigned int base = ldo->dvdd ? WL2866D_DVDD_BASE_UV : WL2866D_AVDD_BASE_UV;
	unsigned int step = ldo->dvdd ? WL2866D_DVDD_STEP_UV : WL2866D_AVDD_STEP_UV;
	unsigned int max_uv_supported = ldo->dvdd ? WL2866D_DVDD_MAX_UV :
		WL2866D_AVDD_MAX_UV;
	unsigned int code;

	if (ldo->default_uv < base || ldo->default_uv > max_uv_supported)
		return -EINVAL;
	code = DIV_ROUND_UP(ldo->default_uv - base, step);
	if (base + code * step != ldo->default_uv || code > U8_MAX)
		return -EINVAL;
	return regmap_write(ldo->chip->map, WL2866D_VSET_BASE + ldo->offset, code);
}

static int wl2866d_add_ldo(struct wl2866d_chip *chip, struct device_node *node)
{
	struct wl2866d_ldo *ldo;
	struct regulator_config cfg = {};
	const char *name;
	u32 offset;
	int ret;

	ret = of_property_read_string(node, "regulator-name", &name);
	if (ret)
		return ret;
	ret = of_property_read_u32(node, "offset", &offset);
	if (ret) {
		u16 offset16;
		ret = of_property_read_u16(node, "offset", &offset16);
		if (ret)
			return ret;
		offset = offset16;
	}
	if (offset > 3)
		return -EINVAL;
	if (strcmp(name, wl2866d_channel_names[offset])) {
		dev_err(chip->dev, "regulator %s has invalid channel offset %u\n",
			name, offset);
		return -EINVAL;
	}

	ldo = devm_kzalloc(chip->dev, sizeof(*ldo), GFP_KERNEL);
	if (!ldo)
		return -ENOMEM;
	ldo->chip = chip;
	ldo->offset = offset;
	ldo->dvdd = offset < 2;
	ldo->default_uv = ldo->dvdd ? 1200000 : 2800000;
	of_property_read_u32(node, "init-voltage", &ldo->default_uv);
	ret = wl2866d_program_init_voltage(ldo);
	if (ret)
		return ret;

	ldo->desc.name = name;
	ldo->desc.of_match = name;
	ldo->desc.type = REGULATOR_VOLTAGE;
	ldo->desc.owner = THIS_MODULE;
	ldo->desc.ops = &wl2866d_ops;
	ldo->desc.n_voltages = 1;

	cfg.dev = chip->dev;
	cfg.driver_data = ldo;
	cfg.of_node = node;
	return PTR_ERR_OR_ZERO(devm_regulator_register(chip->dev, &ldo->desc, &cfg));
}

static int wl2866d_probe(struct i2c_client *client)
{
	struct wl2866d_chip *chip;
	struct device_node *node;
	unsigned int pid;
	int ret;

	chip = devm_kzalloc(&client->dev, sizeof(*chip), GFP_KERNEL);
	if (!chip)
		return -ENOMEM;
	chip->dev = &client->dev;
	chip->map = devm_regmap_init_i2c(client, &wl2866d_regmap_config);
	if (IS_ERR(chip->map))
		return PTR_ERR(chip->map);

	ret = regmap_read(chip->map, 0x00, &pid);
	if (ret)
		return dev_err_probe(chip->dev, ret, "product ID read failed\n");
	dev_info(chip->dev, "WL2866D-compatible device detected, product ID 0x%02x\n", pid);
	ret = regmap_write(chip->map, WL2866D_DISCHARGE_REG, 0xff);
	if (ret)
		return dev_err_probe(chip->dev, ret, "discharge setup failed\n");

	for_each_available_child_of_node(client->dev.of_node, node) {
		ret = wl2866d_add_ldo(chip, node);
		if (ret) {
			of_node_put(node);
			return dev_err_probe(chip->dev, ret, "regulator child registration failed\n");
		}
	}
	i2c_set_clientdata(client, chip);
	return 0;
}

static const struct of_device_id wl2866d_match[] = {
	{ .compatible = "willsemi,wl2866d1" },
	{ .compatible = "willsemi,wl2866d2" },
	{}
};
MODULE_DEVICE_TABLE(of, wl2866d_match);

static struct i2c_driver wl2866d_driver = {
	.driver = { .name = "wl2866d", .of_match_table = wl2866d_match },
	.probe = wl2866d_probe,
};
module_i2c_driver(wl2866d_driver);

MODULE_DESCRIPTION("Willsemi WL2866D regulator driver");
MODULE_LICENSE("GPL");
