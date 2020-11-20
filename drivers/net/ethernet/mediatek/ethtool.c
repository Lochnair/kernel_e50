/*   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; version 2 of the License
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   Copyright (C) 2009-2015 John Crispin <blogic@openwrt.org>
 *   Copyright (C) 2009-2015 Felix Fietkau <nbd@nbd.name>
 *   Copyright (C) 2013-2015 Michael Lee <igvtee@gmail.com>
 */

#include "mtk_eth_soc.h"

#ifdef CONFIG_DTB_UBNT_ER
#include <linux/ubntw.h>
#include <linux/mii.h>
#include <linux/if_vlan.h>
#include <linux/i2c.h>

#include <gsw_mt7620.h>

#include "rtk_switch.h"
#include "rtk_types.h"
#include "port.h"
#endif

static const char fe_gdma_str[][ETH_GSTRING_LEN] = {
#define _FE(x...)	# x,
FE_STAT_REG_DECLARE
#undef _FE
};
#ifdef CONFIG_DTB_UBNT_ER
DEFINE_MUTEX(ethtool_mutex);
static unsigned int sfp_up_old = 0;
static bool sfp_power_switch_required = 0;

static void update_port_status_handler(struct work_struct *work);
struct delayed_work update_port_status_work;

extern struct ubnt_bd_t ubnt_bd_g;
extern struct mutex mdio_nest_lock;
extern struct net_device *dev_raether;
extern struct mt7620_gsw *gsw_mt7621;
extern struct mt7530_priv *switch_priv;
extern rtk_api_ret_t rtk_port_phyAutoNegoAbility_get(rtk_port_t port, rtk_port_phy_ability_t *pAbility);
extern u32 mt7530_r32(struct mt7530_priv *priv, u32 reg);
extern void mt7530_w32(struct mt7530_priv *priv, u32 reg, u32 val);
#endif

int mdio_read(struct net_device *dev, int phy_id, int location)
{
	unsigned int result;
	struct fe_priv *priv = netdev_priv(dev);

	mutex_lock(&mdio_nest_lock);
	if (phy_id == 0x1f) {
		result = mt7530_r32(switch_priv, location);
	} else {
		result = mdiobus_read(priv->mii_bus, phy_id, location);
	}
	mutex_unlock(&mdio_nest_lock);

	return (int)result;
}

void mdio_write(struct net_device *dev, int phy_id, int location, int value)
{
	struct fe_priv *priv = netdev_priv(dev);

	mutex_lock(&mdio_nest_lock);
	if (phy_id == 0x1f) {
		mt7530_w32(switch_priv, location, value);
	} else {
		mdiobus_write(priv->mii_bus, phy_id, location, value);
	}
	mutex_unlock(&mdio_nest_lock);

	return;
}

#ifdef CONFIG_DTB_UBNT_ER
static inline int is_sfp_up(void)
{
	u32 val;
	struct fe_priv *priv = netdev_priv(dev_raether);

	val = priv->mii_info.mdio_read(dev_raether, SFP_PHY_ADDR, MII_BMSR);
	return (val & BMSR_LSTATUS)? 1: 0;
}

static inline int ar8033_get_sfp_spd(void)
{
	u32 val;
	struct fe_priv *priv = netdev_priv(dev_raether);

	val = priv->mii_info.mdio_read(dev_raether, SFP_PHY_ADDR, 0x1f);
	if ((val & 0x000f) == 0x0006)
		return 100;
	else
		return 1000;
}

static int set_sfp_link_status(int carrier)
{
	struct net_device *dev;
	dev = dev_get_by_name(&init_net, SFP_INTF);
	if (!dev)
		return -1;
	else if (!netif_running(dev))
		return -1;

	if (carrier == 1)
		netif_carrier_on(dev);
	else
		netif_carrier_off(dev);

	dev_put(dev);

	return 0;
}

static void update_port_status_handler(struct work_struct *work)
{
	u8 sfp_up = is_sfp_up();

	if (unlikely(sfp_up_old != sfp_up)) {
		if ( !set_sfp_link_status(sfp_up) ) {
			if (sfp_up) { // link up
				/* enable switch port 5 */
				if (ar8033_get_sfp_spd() == 100)
					mt7530_mdio_w32(gsw_mt7621, 0x3500, 0x7e337);
				else
					mt7530_mdio_w32(gsw_mt7621, 0x3500, 0x7e33b);
				printk(KERN_INFO "Port%d Link Up\n", 5);
			} else { // link down
				/* disable switch port 5 */
				mt7530_mdio_w32(gsw_mt7621, 0x3500, 0x8000);
				printk(KERN_INFO "Port%d Link Down\n", 5);
			}
			sfp_up_old = sfp_up;
		}
	}

	if (unlikely(sfp_power_switch_required)) {
		/*
		 * Power down/up every time there's speed change on SFP port.
		 * This makes the whole link re-negotiate.
		 * Currently we add 5 seconds between power down and up becuase it
		 * seems peer may not notice the power-off if we add shorter delay here.
		*/
		u32 bmcr;
		int phy_addr;
		struct fe_priv *priv = netdev_priv(dev_raether);

		sfp_power_switch_required = false;
		phy_addr = SFP_PHY_ADDR;
		bmcr = priv->mii_info.mdio_read(dev_raether, phy_addr, MII_BMCR);
		bmcr |= BMCR_PDOWN;
		priv->mii_info.mdio_write(dev_raether, phy_addr, MII_BMCR, bmcr);
		msleep(5000);
		bmcr &= ~BMCR_PDOWN;
		priv->mii_info.mdio_write(dev_raether, phy_addr, MII_BMCR, bmcr);
	}

	schedule_delayed_work(&update_port_status_work, 2 * HZ);

	return;
}

void ethtool_sfp_init(void) {
	// Add workqueue for SFP status check
	INIT_DELAYED_WORK(&update_port_status_work, update_port_status_handler);
	schedule_delayed_work(&update_port_status_work, 2 * HZ);
}
#endif

void ethtool_init(struct net_device *dev)
{
	struct fe_priv *priv = netdev_priv(dev);

	// init mii structure
	priv->mii_info.dev = dev;
	priv->mii_info.mdio_read = mdio_read;
	priv->mii_info.mdio_write = mdio_write;
	priv->mii_info.phy_id_mask = 0x1f;
	priv->mii_info.reg_num_mask = 0x1f;
	priv->mii_info.supports_gmii = mii_check_gmii_support(&priv->mii_info);
	// TODO:   phy_id: 0~4
	priv->mii_info.phy_id = 31;

	return;
}

static int fe_get_link_ksettings(struct net_device *ndev,
			   struct ethtool_link_ksettings *cmd)
{
	struct fe_priv *priv = netdev_priv(ndev);

	if (!priv->phy_dev)
		return -ENODEV;

	if (priv->phy_flags == FE_PHY_FLAG_ATTACH) {
		if (phy_read_status(priv->phy_dev))
			return -ENODEV;
	}

	phy_ethtool_ksettings_get(ndev->phydev, cmd);

	return 0;
}

static int fe_set_link_ksettings(struct net_device *ndev,
			   const struct ethtool_link_ksettings *cmd)
{
	struct fe_priv *priv = netdev_priv(ndev);

	if (!priv->phy_dev)
		goto out_sset;

	if (cmd->base.phy_address != priv->phy_dev->mdio.addr) {
		if (priv->phy->phy_node[cmd->base.phy_address]) {
			priv->phy_dev = priv->phy->phy[cmd->base.phy_address];
			priv->phy_flags = FE_PHY_FLAG_PORT;
		} else if (priv->mii_bus && mdiobus_get_phy(priv->mii_bus, cmd->base.phy_address)) {
			priv->phy_dev = mdiobus_get_phy(priv->mii_bus, cmd->base.phy_address);
			priv->phy_flags = FE_PHY_FLAG_ATTACH;
		} else {
			goto out_sset;
		}
	}

	return phy_ethtool_ksettings_set(ndev->phydev, cmd);

out_sset:
	return -ENODEV;
}

static void fe_get_drvinfo(struct net_device *dev,
			   struct ethtool_drvinfo *info)
{
	struct fe_priv *priv = netdev_priv(dev);
	struct fe_soc_data *soc = priv->soc;

	strlcpy(info->driver, priv->dev->driver->name, sizeof(info->driver));
	strlcpy(info->version, MTK_FE_DRV_VERSION, sizeof(info->version));
	strlcpy(info->bus_info, dev_name(priv->dev), sizeof(info->bus_info));

	if (soc->reg_table[FE_REG_FE_COUNTER_BASE])
		info->n_stats = ARRAY_SIZE(fe_gdma_str);
}

static u32 fe_get_msglevel(struct net_device *dev)
{
	struct fe_priv *priv = netdev_priv(dev);

	return priv->msg_enable;
}

static void fe_set_msglevel(struct net_device *dev, u32 value)
{
	struct fe_priv *priv = netdev_priv(dev);

	priv->msg_enable = value;
}

static int fe_nway_reset(struct net_device *dev)
{
	struct fe_priv *priv = netdev_priv(dev);

	if (!priv->phy_dev)
		goto out_nway_reset;

	return genphy_restart_aneg(priv->phy_dev);

out_nway_reset:
	return -EOPNOTSUPP;
}

static u32 fe_get_link(struct net_device *dev)
{
	struct fe_priv *priv = netdev_priv(dev);
	int err;

	if (!priv->phy_dev)
		goto out_get_link;

	if (priv->phy_flags == FE_PHY_FLAG_ATTACH) {
		err = genphy_update_link(priv->phy_dev);
		if (err)
			goto out_get_link;
	}

	return priv->phy_dev->link;

out_get_link:
	return ethtool_op_get_link(dev);
}

static int fe_set_ringparam(struct net_device *dev,
			    struct ethtool_ringparam *ring)
{
	struct fe_priv *priv = netdev_priv(dev);

	if ((ring->tx_pending < 2) ||
	    (ring->rx_pending < 2) ||
	    (ring->rx_pending > MAX_DMA_DESC) ||
	    (ring->tx_pending > MAX_DMA_DESC))
		return -EINVAL;

	dev->netdev_ops->ndo_stop(dev);

	priv->tx_ring.tx_ring_size = BIT(fls(ring->tx_pending) - 1);
	priv->rx_ring.rx_ring_size = BIT(fls(ring->rx_pending) - 1);

	dev->netdev_ops->ndo_open(dev);

	return 0;
}

static void fe_get_ringparam(struct net_device *dev,
			     struct ethtool_ringparam *ring)
{
	struct fe_priv *priv = netdev_priv(dev);

	ring->rx_max_pending = MAX_DMA_DESC;
	ring->tx_max_pending = MAX_DMA_DESC;
	ring->rx_pending = priv->rx_ring.rx_ring_size;
	ring->tx_pending = priv->tx_ring.tx_ring_size;
}

static void fe_get_strings(struct net_device *dev, u32 stringset, u8 *data)
{
	switch (stringset) {
	case ETH_SS_STATS:
		memcpy(data, *fe_gdma_str, sizeof(fe_gdma_str));
		break;
	}
}

static int fe_get_sset_count(struct net_device *dev, int sset)
{
	switch (sset) {
	case ETH_SS_STATS:
		return ARRAY_SIZE(fe_gdma_str);
	default:
		return -EOPNOTSUPP;
	}
}

static void fe_get_ethtool_stats(struct net_device *dev,
				 struct ethtool_stats *stats, u64 *data)
{
	struct fe_priv *priv = netdev_priv(dev);
	struct fe_hw_stats *hwstats = priv->hw_stats;
	u64 *data_src, *data_dst;
	unsigned int start;
	int i;

	if (netif_running(dev) && netif_device_present(dev)) {
		if (spin_trylock(&hwstats->stats_lock)) {
			fe_stats_update(priv);
			spin_unlock(&hwstats->stats_lock);
		}
	}

	do {
		data_src = &hwstats->tx_bytes;
		data_dst = data;
		start = u64_stats_fetch_begin_irq(&hwstats->syncp);

		for (i = 0; i < ARRAY_SIZE(fe_gdma_str); i++)
			*data_dst++ = *data_src++;

	} while (u64_stats_fetch_retry_irq(&hwstats->syncp, start));
}

struct ethtool_ops fe_ethtool_ops = {
	.get_link_ksettings	= fe_get_link_ksettings,
	.set_link_ksettings	= fe_set_link_ksettings,
	.get_drvinfo		= fe_get_drvinfo,
	.get_msglevel		= fe_get_msglevel,
	.set_msglevel		= fe_set_msglevel,
	.nway_reset		= fe_nway_reset,
	.get_link		= fe_get_link,
	.set_ringparam		= fe_set_ringparam,
	.get_ringparam		= fe_get_ringparam,
};
EXPORT_SYMBOL(fe_ethtool_ops);

#ifdef CONFIG_DTB_UBNT_ER
#define	SFF_MODULE_ID_SFP	0x3
#define	SFF_MODULE_ID_QSFP	0xc
#define	SFF_MODULE_ID_QSFP_PLUS	0xd
#define	SFF_MODULE_ID_QSFP28	0x11
#define	E50_SFP_PORT_NUM	5
#define	E50_SFP_I2C_BUS		0
#define	I2C_READ_BLOCK_LEN	32
#define	I2C_DEV_ADDR_A0		(0xA0 >> 1)
#define	I2C_DEV_ADDR_A2		(0xA2 >> 1)

static int fe_get_module_info(struct net_device *dev,
		struct ethtool_modinfo *modinfo)
{
	u8 id[2];
	struct i2c_adapter *sys_i2c_adapt = NULL;
	struct i2c_client i2c_sfp_a0;
	struct vlan_dev_priv *vlan = vlan_dev_priv(dev);
	struct fe_priv *priv = netdev_priv(vlan->real_dev);
	int port_num = vlan->vlan_id - PORT_VID_BASE(ubnt_bd_g.type);

	if (strcmp(ubnt_bd_g.type, "e51") || port_num != E50_SFP_PORT_NUM) {
		return -EINVAL;
	}

	if (!priv->phy_dev)
		return -ENETDOWN;

	if (!(sys_i2c_adapt = i2c_get_adapter(E50_SFP_I2C_BUS))) {
		printk(KERN_ERR "Failed to get i2c adapter %d\n",
			E50_SFP_I2C_BUS);
		return -EINVAL;
	}

	memset(&i2c_sfp_a0, 0, sizeof(i2c_sfp_a0));
	strlcpy(i2c_sfp_a0.name, "i2c_sfp_a0", sizeof(i2c_sfp_a0.name));
	i2c_sfp_a0.addr = I2C_DEV_ADDR_A0;
	i2c_sfp_a0.adapter = sys_i2c_adapt;

	/* Read A0h first two bytes, Identifier and Extended Identifier */
	if (i2c_smbus_read_i2c_block_data(&i2c_sfp_a0, 0, 2, id) != 2) {
		// Can't read SFP module identifier, maybe module is not insert?
		return -EINVAL;
	}

	switch (id[0]) {
		case SFF_MODULE_ID_SFP:
			modinfo->type = ETH_MODULE_SFF_8472;
			modinfo->eeprom_len = ETH_MODULE_SFF_8472_LEN;
			break;
		case SFF_MODULE_ID_QSFP:
			modinfo->type = ETH_MODULE_SFF_8436;
			modinfo->eeprom_len = ETH_MODULE_SFF_8436_LEN;
			break;
		case SFF_MODULE_ID_QSFP_PLUS:
			if (id[1] >= 0x3) { /* revision id */
				modinfo->type = ETH_MODULE_SFF_8636;
				modinfo->eeprom_len = ETH_MODULE_SFF_8636_LEN;
			} else {
				modinfo->type = ETH_MODULE_SFF_8436;
				modinfo->eeprom_len = ETH_MODULE_SFF_8436_LEN;
			}
			break;
		case SFF_MODULE_ID_QSFP28:
			modinfo->type = ETH_MODULE_SFF_8636;
			modinfo->eeprom_len = ETH_MODULE_SFF_8636_LEN;
			break;
		default:
			return -EINVAL;
	}

	return 0;
}

static int fe_get_module_eeprom(struct net_device *dev,
		struct ethtool_eeprom *eeprom, u8 *data)
{
	u16 start = eeprom->offset, length = eeprom->len;
	int offset, res;
	struct i2c_adapter *sys_i2c_adapt = NULL;
	struct i2c_client i2c_sfp_a0, i2c_sfp_a2;
	struct vlan_dev_priv *vlan = vlan_dev_priv(dev);
	struct fe_priv *priv = netdev_priv(vlan->real_dev);
	int port_num = vlan->vlan_id - PORT_VID_BASE(ubnt_bd_g.type);

	if (strcmp(ubnt_bd_g.type, "e51") || port_num != E50_SFP_PORT_NUM) {
		return -EINVAL;
	}

	if (!priv->phy_dev)
		return -ENETDOWN;

	if (!(sys_i2c_adapt = i2c_get_adapter(E50_SFP_I2C_BUS))) {
		printk(KERN_ERR "Failed to get i2c adapter %d\n",
			E50_SFP_I2C_BUS);
		return -EINVAL;
	}

	memset(&i2c_sfp_a0, 0, sizeof(i2c_sfp_a0));
	strlcpy(i2c_sfp_a0.name, "i2c_sfp_a0", sizeof(i2c_sfp_a0.name));
	i2c_sfp_a0.addr = I2C_DEV_ADDR_A0;
	i2c_sfp_a0.adapter = sys_i2c_adapt;

	memset(&i2c_sfp_a2, 0, sizeof(i2c_sfp_a2));
	strlcpy(i2c_sfp_a2.name, "i2c_sfp_a2", sizeof(i2c_sfp_a2.name));
	i2c_sfp_a2.addr = I2C_DEV_ADDR_A2;
	i2c_sfp_a2.adapter = sys_i2c_adapt;

	memset(data, 0, eeprom->len);

	/* Read A0 portion of the EEPROM */
	if (start < ETH_MODULE_SFF_8436_LEN) {
		if (start + eeprom->len > ETH_MODULE_SFF_8436_LEN)
			length = ETH_MODULE_SFF_8436_LEN - start;

		for (offset = 0; offset < length; offset += res) {
			res = i2c_smbus_read_i2c_block_data(&i2c_sfp_a0,
				(start + offset), I2C_READ_BLOCK_LEN,
				(data + offset));
			if (res <= 0) {
				printk(KERN_ERR "Failed to read block data, "
					"error %d\n", res);
				return -EINVAL;
			}
		}

		start += length;
		data += length;
		length = eeprom->len - length;
	}

	/* Read A2 portion of the EEPROM */
	if (length) {
		start -= ETH_MODULE_SFF_8436_LEN;

		for (offset = 0; offset < length; offset += res) {
			res = i2c_smbus_read_i2c_block_data(&i2c_sfp_a2,
				(start + offset), I2C_READ_BLOCK_LEN,
				(data + offset));
			if (res <= 0) {
				printk(KERN_ERR "Failed to read block data, "
					"error %d\n", res);
				return -EINVAL;
			}
		}
	}

	return 0;
}

void ethtool_sfp_remove(void) {
	cancel_delayed_work_sync(&update_port_status_work);
}

static inline int ar8033_set_sfp_spd(int speed)
{
	u32 val;
	int phy_addr;
	struct fe_priv *priv = netdev_priv(dev_raether);

	phy_addr = SFP_PHY_ADDR;
	val = priv->mii_info.mdio_read(dev_raether, phy_addr, 0x1f);
	val &= 0xfff0;
	if (speed == 100) {
		val |= 0x0006;
	} else {
		val |= 0x0002;
	}
	priv->mii_info.mdio_write(dev_raether, phy_addr, 0x1f, val);

	return 0;
}

static int fe_get_vif_link_ksettings(struct net_device *ndev,
			   struct ethtool_link_ksettings *cmd)
{
	struct vlan_dev_priv *vlan = vlan_dev_priv(ndev);
	struct fe_priv *priv = netdev_priv(vlan->real_dev);
	u32 bmcr, sfpsr;
	u32 link_status;

	mutex_lock(&ethtool_mutex);

	priv->mii_info.phy_id = vlan->vlan_id - PORT_VID_BASE(ubnt_bd_g.type);

	// Notice - The phy id of fiber port (eth5) is 7, not 5 !!!!!
	if ( priv->mii_info.phy_id == 5 && !strcmp(ubnt_bd_g.type, "e51") )
		priv->mii_info.phy_id = SFP_PHY_ADDR;

	mii_ethtool_get_link_ksettings(&priv->mii_info, cmd);

	// ER-X SFP fiber port
	if ( priv->mii_info.phy_id == SFP_PHY_ADDR && !strcmp(ubnt_bd_g.type, "e51") ) {
		bmcr = priv->mii_info.mdio_read(dev_raether, SFP_PHY_ADDR, MII_BMCR);
		sfpsr = priv->mii_info.mdio_read(dev_raether, SFP_PHY_ADDR, 0x11);

		cmd->base.autoneg = (bmcr & BMCR_ANENABLE) ? 1 : 0;
		link_status = (sfpsr & (1 << 10)) ? 1 : 0;

		switch (sfpsr >> 14) {
			case 2:
				cmd->base.speed = 1000;
				break;
			case 1:
				cmd->base.speed = 100;
				break;
			case 0:
			default:
				cmd->base.speed = 10;
				break;
		}
		cmd->base.duplex = (sfpsr & (1 << 13)) ? 1: 0;
	}

	mutex_unlock(&ethtool_mutex);

	return 0;
}

static int fe_set_vif_link_ksettings(struct net_device *ndev,
			   const struct ethtool_link_ksettings *cmd)
{
	struct vlan_dev_priv *vlan = vlan_dev_priv(ndev);
	struct fe_priv *priv = netdev_priv(vlan->real_dev);
	int rc = 0;

	mutex_lock(&ethtool_mutex);
	priv->mii_info.phy_id = vlan->vlan_id - PORT_VID_BASE(ubnt_bd_g.type);

	/*
	 * Power down/up every time there's speed change on SFP port.
	 * This makes the whole link re-negotiate.
	 * Currently we add 5 seconds between power down and up becuase it
	 * seems peer may not notice the power-off if we add shorter delay here.
	 */
	if ( priv->mii_info.phy_id == 5 && !strcmp(ubnt_bd_g.type, "e51") ) { // ER-X SFP
		priv->mii_info.phy_id = SFP_PHY_ADDR;
		sfp_power_switch_required = true;
	}

	rc = mii_ethtool_set_link_ksettings(&priv->mii_info, cmd);

	mutex_unlock(&ethtool_mutex);

	return rc;
}

static void fe_get_vif_drvinfo(struct net_device *ndev,
			   struct ethtool_drvinfo *info)
{
	struct vlan_dev_priv *vlan = vlan_dev_priv(ndev);
	struct fe_priv *priv = netdev_priv(vlan->real_dev);
	struct fe_soc_data *soc = priv->soc;

	strlcpy(info->driver, priv->dev->driver->name, sizeof(info->driver));
	strlcpy(info->version, MTK_FE_DRV_VERSION, sizeof(info->version));
	strlcpy(info->bus_info, dev_name(priv->dev), sizeof(info->bus_info));

	if (soc->reg_table[FE_REG_FE_COUNTER_BASE])
		info->n_stats = ARRAY_SIZE(fe_gdma_str);
}

static u32 fe_get_vif_link(struct net_device *dev)
{
	return ethtool_op_get_link(dev);
}

static void fe_get_vif_ethtool_stats(struct net_device *dev,
				 struct ethtool_stats *stats, u64 *data)
{
	struct vlan_dev_priv *vlan = vlan_dev_priv(dev);
	struct fe_priv *priv = netdev_priv(vlan->real_dev);
	struct fe_hw_stats *hwstats = priv->hw_stats;
	u64 *data_src, *data_dst;
	unsigned int start;
	int i;

	if (netif_running(dev) && netif_device_present(dev)) {
		if (spin_trylock(&hwstats->stats_lock)) {
			fe_stats_update(priv);
			spin_unlock(&hwstats->stats_lock);
		}
	}

	do {
		data_src = &hwstats->tx_bytes;
		data_dst = data;
		start = u64_stats_fetch_begin_irq(&hwstats->syncp);

		for (i = 0; i < ARRAY_SIZE(fe_gdma_str); i++)
			*data_dst++ = *data_src++;

	} while (u64_stats_fetch_retry_irq(&hwstats->syncp, start));
}

struct ethtool_ops fe_vif_ethtool_ops = {
	.get_link_ksettings	= fe_get_vif_link_ksettings,
	.set_link_ksettings	= fe_set_vif_link_ksettings,
	.get_drvinfo		= fe_get_vif_drvinfo,
	.get_link			= fe_get_vif_link,
	.get_module_info	= fe_get_module_info,
	.get_module_eeprom	= fe_get_module_eeprom,
};
EXPORT_SYMBOL(fe_vif_ethtool_ops);

static int rtk_get_link_ksettings(struct net_device *ndev,
			   struct ethtool_link_ksettings *cmd)
{
	struct vlan_dev_priv *vlan = vlan_dev_priv(ndev);
	struct fe_priv *priv = netdev_priv(vlan->real_dev);

	u32 advertising;
	rtk_port_phy_ability_t ability;
	rtk_port_linkStatus_t pLinkStatus;
	rtk_port_speed_t pSpeed;
	rtk_port_duplex_t pDuplex;

	mutex_lock(&ethtool_mutex);

	priv->mii_info.phy_id = vlan->vlan_id - PORT_VID_BASE(ubnt_bd_g.type);
	mii_ethtool_get_link_ksettings(&priv->mii_info, cmd);

	advertising = ADVERTISED_TP | ADVERTISED_MII;

	rtk_port_phyAutoNegoAbility_get(priv->mii_info.phy_id - P5_PHY_ID, &ability);

	advertising = 0;
	if ( ability.AutoNegotiation == ENABLED ) {
		advertising |= ADVERTISED_Autoneg;
		cmd->base.autoneg = AUTONEG_ENABLE;
	}
	if (ability.Half_10 == ENABLED)
		advertising |= ADVERTISED_10baseT_Half;
	if (ability.Full_10 == ENABLED)
		advertising |= ADVERTISED_10baseT_Full;
	if (ability.Half_100 == ENABLED)
		advertising |= ADVERTISED_100baseT_Half;
	if (ability.Full_100 == ENABLED)
		advertising |= ADVERTISED_100baseT_Full;
	if (ability.Full_1000 == ENABLED)
		advertising |= ADVERTISED_1000baseT_Full;

	rtk_port_phyStatus_get(priv->mii_info.phy_id - P5_PHY_ID, &pLinkStatus, &pSpeed, &pDuplex);

	switch(pSpeed) {
		case PORT_SPEED_10M:
			cmd->base.speed = SPEED_10;
			break;
		case PORT_SPEED_100M:
			cmd->base.speed = SPEED_100;
			break;
		case PORT_SPEED_1000M:
			cmd->base.speed = SPEED_1000;
			break;
		default:
			cmd->base.speed = SPEED_10;
	}

	cmd->base.duplex = (pDuplex == PORT_HALF_DUPLEX) ? (DUPLEX_HALF) : (DUPLEX_FULL);

	ethtool_convert_legacy_u32_to_link_mode(cmd->link_modes.advertising,
						advertising);

	mutex_unlock(&ethtool_mutex);

	return 0;
}

static int rtk_set_link_ksettings(struct net_device *ndev,
			   const struct ethtool_link_ksettings *cmd)
{
	struct vlan_dev_priv *vlan = vlan_dev_priv(ndev);
	struct fe_priv *priv = netdev_priv(vlan->real_dev);
	rtk_port_phy_ability_t ability, org_ability;
	rtk_port_t port;
	int rc = 0;

	mutex_lock(&ethtool_mutex);

	priv->mii_info.phy_id = vlan->vlan_id - PORT_VID_BASE(ubnt_bd_g.type);
	port = priv->mii_info.phy_id - P5_PHY_ID;

	memset(&ability, 0, sizeof(rtk_port_phy_ability_t));
	rtk_port_phyAutoNegoAbility_get(port, &org_ability);

	// Sync up flow control settings
	ability.AsyFC = org_ability.AsyFC;
	ability.FC = org_ability.FC;

	if (cmd->base.autoneg == AUTONEG_ENABLE) {
		u32 advertising = 0;

		ethtool_convert_link_mode_to_legacy_u32(
			&advertising, cmd->link_modes.advertising);

		// RTL8367 doesn't support 1000-half when AN=on
		if ((advertising & (ADVERTISED_10baseT_Half |
				    ADVERTISED_10baseT_Full |
				    ADVERTISED_100baseT_Half |
				    ADVERTISED_100baseT_Full |
				    ADVERTISED_1000baseT_Full)) == 0) {
			goto RTL8367_KSETTING_ERR;
		}

		if (advertising & ADVERTISED_10baseT_Full)
			ability.Full_10 = 1;
		if (advertising & ADVERTISED_10baseT_Half)
			ability.Half_10 = 1;
		if (advertising & ADVERTISED_100baseT_Full)
			ability.Full_100 = 1;
		if (advertising & ADVERTISED_100baseT_Half)
			ability.Half_100 = 1;
		if (advertising & ADVERTISED_1000baseT_Full)
			ability.Full_1000 = 1;

		ability.AutoNegotiation = ENABLED;

		rc = rtk_port_phyAutoNegoAbility_set(port, &ability);
	} else {
		ability.AutoNegotiation = DISABLED;

		switch (cmd->base.speed) {
			case SPEED_10:
				(cmd->base.duplex == DUPLEX_HALF) ? ( ability.Half_10 = 1 ) : ( ability.Full_10 = 1 );
				break;
			case SPEED_100:
				(cmd->base.duplex == DUPLEX_HALF) ? ( ability.Half_100 = 1 ) : ( ability.Full_100 = 1 );
				break;
			case SPEED_1000:
				// RLT8367 doesn't support speed 1000 when AN=off
				goto RTL8367_KSETTING_ERR;
			default:
				ability.Full_100 = 1;
		}

		rc = rtk_port_phyForceModeAbility_set(port, &ability);
	}

	mutex_unlock(&ethtool_mutex);

	return rc;

RTL8367_KSETTING_ERR:
	mutex_unlock(&ethtool_mutex);

	return -EINVAL;
}

static void rtk_get_drvinfo(struct net_device *ndev,
			   struct ethtool_drvinfo *info)
{
	strlcpy(info->driver, "rtl8367c", sizeof(info->driver));
	strlcpy(info->version, "1.3.12", sizeof(info->version));
}

u32 rtk_get_link(struct net_device *dev)
{
	struct vlan_dev_priv *vlan = vlan_dev_priv(dev);
	struct fe_priv *priv = netdev_priv(vlan->real_dev);
	int err;

	rtk_port_linkStatus_t pLinkStatus;
	rtk_port_speed_t pSpeed;
	rtk_port_duplex_t pDuplex;

	mutex_lock(&ethtool_mutex);
	priv->mii_info.phy_id = vlan->vlan_id - PORT_VID_BASE(ubnt_bd_g.type);
	err = rtk_port_phyStatus_get(priv->mii_info.phy_id - P5_PHY_ID, &pLinkStatus, &pSpeed, &pDuplex);
	mutex_unlock(&ethtool_mutex);

	if (err)
		goto out_get_link;

	return pLinkStatus;

out_get_link:
	return ethtool_op_get_link(dev);
}
EXPORT_SYMBOL(rtk_get_link);

struct ethtool_ops rtk_vif_ethtool_ops = {
	.get_link_ksettings	= rtk_get_link_ksettings,
	.set_link_ksettings	= rtk_set_link_ksettings,
	.get_drvinfo		= rtk_get_drvinfo,
	.get_link			= rtk_get_link,
};
EXPORT_SYMBOL(rtk_vif_ethtool_ops);
#endif

void fe_set_ethtool_ops(struct net_device *netdev)
{
	struct fe_priv *priv = netdev_priv(netdev);
	struct fe_soc_data *soc = priv->soc;

	if (soc->reg_table[FE_REG_FE_COUNTER_BASE]) {
		fe_ethtool_ops.get_strings = fe_get_strings;
		fe_ethtool_ops.get_sset_count = fe_get_sset_count;
		fe_ethtool_ops.get_ethtool_stats = fe_get_ethtool_stats;
#ifdef CONFIG_DTB_UBNT_ER
		fe_vif_ethtool_ops.get_strings = fe_get_strings;
		fe_vif_ethtool_ops.get_sset_count = fe_get_sset_count;
		fe_vif_ethtool_ops.get_ethtool_stats = fe_get_vif_ethtool_stats;
#endif
	}

	netdev->ethtool_ops = &fe_ethtool_ops;
}

