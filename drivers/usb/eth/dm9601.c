/*
 * Davicom DM96xx USB 10/100Mbps ethernet devices
 *
 * Peter Korsgaard <jacmet@sunsite.dk>
 *
 * This file is licensed under the terms of the GNU General Public License
 * version 2.  This program is licensed "as is" without any warranty of any
 * kind, whether express or implied.
 */

//#define DEBUG

#if 0
#include <linux/module.h>
#include <linux/sched.h>
#include <linux/stddef.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/ethtool.h>
#include <linux/mii.h>
#include <linux/usb.h>
#include <linux/crc32.h>
#include <linux/usb/usbnet.h>
#include <linux/slab.h>
#endif

#include <common.h>
#include <usb.h>
#include <linux/mii.h>
#include "usb_ether.h"
#include <malloc.h>
#include <errno.h>

/* datasheet:
 http://ptm2.cc.utu.fi/ftp/network/cards/DM9601/From_NET/DM9601-DS-P01-930914.pdf
*/

#define DM9601_BASE_NAME "Davicom DM96xx USB 10/100 Ethernet"

struct dm_dongle {
	unsigned short vendor;
	unsigned short product;
	int flags;
};

static const struct dm_dongle products[] = {
	{0x07aa, 0x9601, 0, }, /* Corega FEther USB-TXC */
	{0x0a46, 0x9601, 0, }, /* Davicom USB-100 */
	{0x0a46, 0x6688, 0, }, /* ZT6688 USB NIC */
	{0x0a46, 0x0268, 0, }, /* ShanTou ST268 USB NIC */
	{0x0a46, 0x8515, 0, }, /* ADMtek ADM8515 USB NIC */
	{0x0a47, 0x9601, 0, }, /* Hirose USB-100 */
	{0x0fe6, 0x8101, 0, }, /* DM9601 USB to Fast Ethernet Adapter */
	{0x0fe6, 0x9700, 0, }, /* DM9601 USB to Fast Ethernet Adapter */
	{0x0a46, 0x9000, 0, }, /* DM9000E */
	{0x0a46, 0x9620, 0, }, /* DM9620 USB to Fast Ethernet Adapter */
	{0x0a46, 0x9621, 0, }, /* DM9621A USB to Fast Ethernet Adapter */
	{0x0a46, 0x9622, 0, }, /* DM9622 USB to Fast Ethernet Adapter */
	{0x0a46, 0x0269, 0, }, /* DM962OA USB to Fast Ethernet Adapter */
	{0x0a46, 0x1269, 0, }, /* DM9621A USB to Fast Ethernet Adapter */
	{},			/* END */
};


/* control requests */
#define DM_READ_REGS	0x00
#define DM_WRITE_REGS	0x01
#define DM_READ_MEMS	0x02
#define DM_WRITE_REG	0x03
#define DM_WRITE_MEMS	0x05
#define DM_WRITE_MEM	0x07

/* registers */
#define DM_NET_CTRL	0x00
#define DM_RX_CTRL	0x05
#define DM_SHARED_CTRL	0x0b
#define DM_SHARED_ADDR	0x0c
#define DM_SHARED_DATA	0x0d	/* low + high */
#define DM_PHY_ADDR	0x10	/* 6 bytes */
#define DM_MCAST_ADDR	0x16	/* 8 bytes */
#define DM_GPR_CTRL	0x1e
#define DM_GPR_DATA	0x1f
#define DM_CHIP_ID	0x2c
#define DM_MODE_CTRL	0x91	/* only on dm9620 */

/* chip id values */
#define ID_DM9601	0
#define ID_DM9620	1

#define DM_MAX_MCAST	64
#define DM_MCAST_SIZE	8
#define DM_EEPROM_LEN	256
#define DM_TX_OVERHEAD	2	/* 2 byte header */
#define DM_RX_OVERHEAD	7	/* 3 byte header + 4 byte crc tail */
#define DM_TIMEOUT	1000

#define USB_CTRL_SET_TIMEOUT 5000
#define USB_CTRL_GET_TIMEOUT 5000
#define USB_BULK_SEND_TIMEOUT 5000
#define USB_BULK_RECV_TIMEOUT 5000

#define AX_RX_URB_SIZE 2048
#define PHY_CONNECT_TIMEOUT 5000

static int dm_read(struct ueth_data *dev, u8 reg, u16 length, void *data)
{
	int len;
	len = usb_control_msg(
		dev->pusb_dev,
		usb_sndctrlpipe(dev->pusb_dev, 0),
		DM_READ_REGS,
		USB_DIR_OUT | USB_TYPE_VENDOR | USB_RECIP_DEVICE,
		0,
		reg,
		data,
		length,
		USB_CTRL_SET_TIMEOUT);

	return len == length ? 0 : -EINVAL;
}

static int dm_read_reg(struct ueth_data *dev, u8 reg, u8 *value)
{
	return dm_read(dev, reg, 1, value);
}

static int dm_write(struct ueth_data *dev, u8 reg, u16 length, void *data)
{
	int len;
	len = usb_control_msg(
		dev->pusb_dev,
		usb_rcvctrlpipe(dev->pusb_dev, 0),
		DM_WRITE_REGS,
		USB_DIR_OUT | USB_TYPE_VENDOR | USB_RECIP_DEVICE,
		0,
		reg,
		data,
		length,
		USB_CTRL_SET_TIMEOUT);
	return len == length ? 0 : -EINVAL;
}

static int dm_write_reg(struct ueth_data *dev, u8 reg, u8 value)
{
	return usb_control_msg(
		dev->pusb_dev,
		usb_rcvctrlpipe(dev->pusb_dev, 0),
		DM_WRITE_REG,
		USB_DIR_OUT | USB_TYPE_VENDOR | USB_RECIP_DEVICE,
		value,
		reg,
		NULL,
		0,
		USB_CTRL_SET_TIMEOUT);
}


static int dm_read_shared_word(struct ueth_data *dev, int phy, u8 reg, __le16 *value)
{
	int ret, i;

	/*mutex_lock(&dev->phy_mutex);*/

	dm_write_reg(dev, DM_SHARED_ADDR, phy ? (reg | 0x40) : reg);
	dm_write_reg(dev, DM_SHARED_CTRL, phy ? 0xc : 0x4);

	for (i = 0; i < DM_TIMEOUT; i++) {
		u8 tmp = 0;

		udelay(1);
		ret = dm_read_reg(dev, DM_SHARED_CTRL, &tmp);
		if (ret < 0)
			goto out;

		/* ready */
		if ((tmp & 1) == 0)
			break;
	}

	if (i == DM_TIMEOUT) {
		debug("%s read timed out!\n", phy ? "phy" : "eeprom");
		ret = -EIO;
		goto out;
	}

	dm_write_reg(dev, DM_SHARED_CTRL, 0x0);
	ret = dm_read(dev, DM_SHARED_DATA, 2, value);

	debug("read shared %d 0x%02x returned 0x%04x, %d\n",
		   phy, reg, *value, ret);

 out:
	/*mutex_unlock(&dev->phy_mutex);*/
	return ret;
}


static int dm_write_shared_word(struct ueth_data *dev, int phy, u8 reg, __le16 value)
{
	int ret, i;

	/*mutex_lock(&dev->phy_mutex);*/

	ret = dm_write(dev, DM_SHARED_DATA, 2, &value);
	if (ret < 0)
		goto out;

	dm_write_reg(dev, DM_SHARED_ADDR, phy ? (reg | 0x40) : reg);
	dm_write_reg(dev, DM_SHARED_CTRL, phy ? 0x1a : 0x12);

	for (i = 0; i < DM_TIMEOUT; i++) {
		u8 tmp = 0;

		udelay(1);
		ret = dm_read_reg(dev, DM_SHARED_CTRL, &tmp);
		if (ret < 0)
			goto out;

		/* ready */
		if ((tmp & 1) == 0)
			break;
	}

	if (i == DM_TIMEOUT) {
		debug("%s write timed out!\n", phy ? "phy" : "eeprom");
		ret = -EIO;
		goto out;
	}

	dm_write_reg(dev, DM_SHARED_CTRL, 0x0);

out:
	/*mutex_unlock(&dev->phy_mutex);*/
	return ret;
}


#if 0
static int dm_read_eeprom_word(struct ueth_data *dev, u8 offset, void *value)
{
	return dm_read_shared_word(dev, 0, offset, value);
}


static int dm9601_get_eeprom_len(struct net_device *dev)
{
	return DM_EEPROM_LEN;
}


static int dm9601_get_eeprom(struct net_device *net,
			     struct ethtool_eeprom *eeprom, u8 * data)
{
	struct usbnet *dev = netdev_priv(net);
	__le16 *ebuf = (__le16 *) data;
	int i;

	/* access is 16bit */
	if ((eeprom->offset % 2) || (eeprom->len % 2))
		return -EINVAL;

	for (i = 0; i < eeprom->len / 2; i++) {
		if (dm_read_eeprom_word(dev, eeprom->offset / 2 + i,
					&ebuf[i]) < 0)
			return -EINVAL;
	}
	return 0;
}
#endif


static int dm9601_mdio_read(struct ueth_data *dev, int phy_id, int loc)
{
	__le16 res;

	if (phy_id) {
		debug("Only internal phy supported\n");
		return 0;
	}

	dm_read_shared_word(dev, 1, loc, &res);

	debug("dm9601_mdio_read() phy_id=0x%02x, loc=0x%02x, returns=0x%04x\n",
		  phy_id, loc, le16_to_cpu(res));

	return le16_to_cpu(res);
}


static void dm9601_mdio_write(struct ueth_data *dev, int phy_id, int loc,
			      int val)
{
	__le16 res = cpu_to_le16(val);

	if (phy_id) {
		debug("Only internal phy supported\n");
		return;
	}

	debug("dm9601_mdio_write() phy_id=0x%02x, loc=0x%02x, val=0x%04x\n",
		   phy_id, loc, val);

	dm_write_shared_word(dev, 1, loc, res);
}


#if 0
static void dm9601_get_drvinfo(struct net_device *net,
			       struct ethtool_drvinfo *info)
{
	/* Inherit standard device info */
	usbnet_get_drvinfo(net, info);
	info->eedump_len = DM_EEPROM_LEN;
}

static u32 dm9601_get_link(struct net_device *net)
{
	struct usbnet *dev = netdev_priv(net);

	return mii_link_ok(&dev->mii);
}

static int dm9601_ioctl(struct net_device *net, struct ifreq *rq, int cmd)
{
	struct usbnet *dev = netdev_priv(net);

	return generic_mii_ioctl(&dev->mii, if_mii(rq), cmd, NULL);
}
#endif


static void __dm9601_set_mac_address(struct ueth_data *dev)
{
	dm_write /*_async*/(dev, DM_PHY_ADDR, ETH_ALEN, dev->eth_dev.enetaddr);
}


static int dm9601_set_mac_address(struct eth_device *eth)
{
	struct ueth_data *dev = (struct ueth_data *)eth->priv;

	if (!is_valid_ether_addr(eth->enetaddr)) {
		debug("not setting invalid mac address %pM\n",
								eth->enetaddr);
		return -EINVAL;
	}

	__dm9601_set_mac_address(dev);

	return 0;
}


static int dm9601_read_mac_address(struct eth_device *eth)
{
	struct ueth_data *dev = (struct ueth_data *)eth->priv;

	/* read MAC */
	if (dm_read(dev, DM_PHY_ADDR, ETH_ALEN, eth->enetaddr) < 0) {
		debug("Error reading MAC address\n");
		return -ENODEV;
	}

	return 0;
}


static void dm9601_set_multicast(struct eth_device *eth)
{
	struct ueth_data *dev = (struct ueth_data *)eth->priv;
	/* We use the 20 byte dev->data for our 8 byte filter buffer
	 * to avoid allocating memory that is tricky to free later */
	u8 hashes[DM_MCAST_SIZE];
	u8 rx_ctl = 0x31;

	memset(hashes, 0x00, DM_MCAST_SIZE);
	hashes[DM_MCAST_SIZE - 1] |= 0x80;	/* broadcast address */

#if 0
	if (net->flags & IFF_PROMISC) {
		rx_ctl |= 0x02;
	} else if (net->flags & IFF_ALLMULTI ||
		   netdev_mc_count(net) > DM_MAX_MCAST) {
		rx_ctl |= 0x08;
	} else if (!netdev_mc_empty(net)) {
		struct netdev_hw_addr *ha;

		netdev_for_each_mc_addr(ha, net) {
			u32 crc = ether_crc(ETH_ALEN, ha->addr) >> 26;
			hashes[crc >> 3] |= 1 << (crc & 0x7);
		}
	}
#endif

	dm_write(dev, DM_MCAST_ADDR, DM_MCAST_SIZE, hashes);
	dm_write_reg(dev, DM_RX_CTRL, rx_ctl);
}


/*
 * mii_nway_restart - restart NWay (autonegotiation) for this interface
 *
 * Returns 0 on success, negative on error.
 */
static int mii_nway_restart(struct ueth_data *dev)
{
	int bmcr;
	int r = -1;

	/* if autoneg is off, it's an error */
	bmcr = dm9601_mdio_read(dev, dev->phy_id, MII_BMCR);

	if (bmcr & BMCR_ANENABLE) {
		bmcr |= BMCR_ANRESTART;
		dm9601_mdio_write(dev, dev->phy_id, MII_BMCR, bmcr);
		r = 0;
	}

	return r;
}


static int dm9601_init(struct eth_device *eth, bd_t *bd)
{
	u8 id = 0;
	struct ueth_data *dev = (struct ueth_data *)eth->priv;

	if (dm_read_reg(dev, DM_CHIP_ID, &id) < 0) {
		debug("Error reading chip ID\n");
		return 0;
	}

	/* put dm9620 devices in dm9601 mode */
	if (id == ID_DM9620) {
		u8 mode;

		if (dm_read_reg(dev, DM_MODE_CTRL, &mode) < 0) {
			debug("Error reading MODE_CTRL\n");
			return 0;
		}
		dm_write_reg(dev, DM_MODE_CTRL, mode & 0x7f);
	}

	/* power up phy */
	dm_write_reg(dev, DM_GPR_CTRL, 1);
	dm_write_reg(dev, DM_GPR_DATA, 0);

	/* receive broadcast packets */
	dm9601_set_multicast(eth);

	dm9601_mdio_write(dev, dev->phy_id, MII_BMCR, BMCR_RESET);
	dm9601_mdio_write(dev, dev->phy_id, MII_ADVERTISE,
			  ADVERTISE_ALL | ADVERTISE_CSMA | ADVERTISE_PAUSE_CAP);

	mii_nway_restart(dev);

	return 1;
}


static int dm9601_send(struct eth_device *eth, void *packet, int length)
{
	struct ueth_data *dev = (struct ueth_data *)eth->priv;
	int err;
	u16 packet_len;
	int actual_len;
	ALLOC_CACHE_ALIGN_BUFFER(unsigned char, msg,
		PKTSIZE + sizeof(packet_len));

	debug("** %s(), len %d\n", __func__, length);

	/* format:
	   b1: packet length low
	   b2: packet length high
	   b3..n: packet data
	*/

	packet_len = (((length) ^ 0x000000ff) << 8) + (length);
	cpu_to_le16s(&packet_len);

	memcpy(msg, &packet_len, sizeof(packet_len));
	memcpy(msg + sizeof(packet_len), (void *)packet, length);

	err = usb_bulk_msg(dev->pusb_dev,
				usb_sndbulkpipe(dev->pusb_dev, dev->ep_out),
				(void *)msg,
				length + sizeof(packet_len),
				&actual_len,
				USB_BULK_SEND_TIMEOUT);
	debug("Tx: len = %u, actual = %u, err = %d\n",
			length + sizeof(packet_len), actual_len, err);

	return err;
}


static int dm9601_recv(struct eth_device *eth)
{
	struct ueth_data *dev = (struct ueth_data *)eth->priv;
	ALLOC_CACHE_ALIGN_BUFFER(unsigned char, recv_buf, AX_RX_URB_SIZE);
	unsigned char *buf_ptr;
	int err;
	int actual_len;
	u16 packet_len;
	u8 status;

	debug("** %s()\n", __func__);

	/* format:
	   b1: rx status
	   b2: packet length (incl crc) low
	   b3: packet length (incl crc) high
	   b4..n-4: packet data
	   bn-3..bn: ethernet crc
	 */

	err = usb_bulk_msg(dev->pusb_dev,
				usb_rcvbulkpipe(dev->pusb_dev, dev->ep_in),
				(void *)recv_buf,
				AX_RX_URB_SIZE,
				&actual_len,
				USB_BULK_RECV_TIMEOUT);
	debug("Rx: len = %u, actual = %u, err = %d\n", AX_RX_URB_SIZE,
		actual_len, err);
	if (err != 0) {
		debug("Rx: failed to receive\n");
		return -1;
	}
	if (actual_len > AX_RX_URB_SIZE) {
		debug("Rx: received too many bytes %d\n", actual_len);
		return -1;
	}

	buf_ptr = recv_buf;
	while (actual_len > 0) {
		/*
		 * First byte contains packet status.
		 */
		if (actual_len < sizeof(status)) {
			debug("Rx: incomplete packet length (status)\n");
			return -1;
		}
		status = buf_ptr[0];
		buf_ptr += sizeof(status);
		actual_len -= sizeof(status);

		if (unlikely(status & 0xbf)) {
			debug("Rx: packet status failure: %d\n", (int)status);
			/*
			if (status & 0x01) dev->net->stats.rx_fifo_errors++;
			if (status & 0x02) dev->net->stats.rx_crc_errors++;
			if (status & 0x04) dev->net->stats.rx_frame_errors++;
			if (status & 0x20) dev->net->stats.rx_missed_errors++;
			if (status & 0x90) dev->net->stats.rx_length_errors++;
			*/
			return -1;
		}

		/*
		 * 2nd and 3rd bytes contain the length of the actual data.
		 * Extract the length of the data.
		 */
		if (actual_len < sizeof(packet_len)) {
			debug("Rx: incomplete packet length (size)\n");
			return -1;
		}
		memcpy(&packet_len, buf_ptr, sizeof(packet_len));
		le16_to_cpus(&packet_len);
		buf_ptr += sizeof(packet_len);
		actual_len -= sizeof(packet_len);

		if (((~packet_len >> 16) & 0x7ff) != (packet_len & 0x7ff)) {
			debug("Rx: malformed packet length: %#x (%#x:%#x)\n",
			      packet_len, (~packet_len >> 16) & 0x7ff,
			      packet_len & 0x7ff);
			return -1;
		}
		packet_len = packet_len & 0x7ff;
		if (packet_len > actual_len) {
			debug("Rx: too large packet: %d\n", packet_len);
			return -1;
		}

		/* Notify net stack */
		NetReceive(buf_ptr, packet_len);

		/* Adjust for next iteration. Packets are padded to 16-bits */
		if (packet_len & 1)
			packet_len++;
		actual_len -= packet_len;
		buf_ptr += packet_len;
	}

	return err;
}


static void dm9601_halt(struct eth_device *eth)
{
	debug("** %s()\n", __func__);
}


void dm9601_eth_before_probe(void)
{
	debug("** %s()\n", __func__);
}


int dm9601_eth_probe(struct usb_device *dev, unsigned int ifnum,
		struct ueth_data* ss)
{
	struct usb_interface *iface;
	struct usb_interface_descriptor *iface_desc;
	int ep_in_found = 0, ep_out_found = 0;
	int i;

	/* let's examine the device now */
	iface = &dev->config.if_desc[ifnum];
	iface_desc = &dev->config.if_desc[ifnum].desc;

	for (i = 0; products[i].vendor != 0; i++) {
		if (dev->descriptor.idVendor == products[i].vendor &&
		    dev->descriptor.idProduct == products[i].product)
			/* Found a supported dongle */
			break;
	}

	if (products[i].vendor == 0)
		return 0;

	memset(ss, 0, sizeof(struct ueth_data));

	/* At this point, we know we've got a live one */
	debug("\n\nUSB Ethernet device detected: %#04x:%#04x\n",
	      dev->descriptor.idVendor, dev->descriptor.idProduct);

	/* Initialize the ueth_data structure with some useful info */
	ss->ifnum = ifnum;
	ss->pusb_dev = dev;
	ss->subclass = iface_desc->bInterfaceSubClass;
	ss->protocol = iface_desc->bInterfaceProtocol;

	/*
	 * We are expecting a minimum of 3 endpoints - in, out (bulk), and
	 * int. We will ignore any others.
	 */
	for (i = 0; i < iface_desc->bNumEndpoints; i++) {
		/* is it an BULK endpoint? */
		if ((iface->ep_desc[i].bmAttributes &
		     USB_ENDPOINT_XFERTYPE_MASK) == USB_ENDPOINT_XFER_BULK) {
			u8 ep_addr = iface->ep_desc[i].bEndpointAddress;
			if (ep_addr & USB_DIR_IN) {
				if (!ep_in_found) {
					ss->ep_in = ep_addr &
						USB_ENDPOINT_NUMBER_MASK;
					ep_in_found = 1;
				}
			} else {
				if (!ep_out_found) {
					ss->ep_out = ep_addr &
						USB_ENDPOINT_NUMBER_MASK;
					ep_out_found = 1;
				}
			}
		}

		/* is it an interrupt endpoint? */
		if ((iface->ep_desc[i].bmAttributes &
		    USB_ENDPOINT_XFERTYPE_MASK) == USB_ENDPOINT_XFER_INT) {
			ss->ep_int = iface->ep_desc[i].bEndpointAddress &
				USB_ENDPOINT_NUMBER_MASK;
			ss->irqinterval = iface->ep_desc[i].bInterval;
		}
	}
	debug("Endpoints In %d Out %d Int %d\n",
		  ss->ep_in, ss->ep_out, ss->ep_int);

	/* Do some basic sanity checks, and bail if we find a problem */
	if (usb_set_interface(dev, iface_desc->bInterfaceNumber, 0) ||
	    !ss->ep_in || !ss->ep_out || !ss->ep_int) {
		debug("Problems with device\n");
		return 0;
	}

	return 1;
}


int dm9601_eth_get_info(struct usb_device *usb_dev, struct ueth_data *ss,
				struct eth_device *eth)
{
	struct ueth_data *dev = (struct ueth_data *)eth->priv;

	if (!eth) {
		debug("%s: missing parameter.\n", __func__);
		return 0;
	}

	sprintf(eth->name, "%s%d", DM9601_BASE_NAME, 0 /*curr_eth_dev++*/);
	eth->init = dm9601_init;
	eth->send = dm9601_send;
	eth->recv = dm9601_recv;
	eth->halt = dm9601_halt;
#ifdef CONFIG_MCAST_TFTP
	/*
	eth->mcast = dm9601_mcast(struct eth_device *, const u8 *enetaddr, u8 set);
	*/
#endif
	eth->write_hwaddr = dm9601_set_mac_address;
	eth->priv = ss;

	/* Get the MAC address */

	/* reset */
	dm_write_reg(dev, DM_NET_CTRL, 1);
	udelay(20);

	/* read MAC */
	if (dm9601_read_mac_address(eth))
		return 0;
	debug("MAC %pM\n", eth->enetaddr);

	/*
	 * Overwrite the auto-generated address only with good ones.
	 */
	/*if (is_valid_ether_addr(mac))
		memcpy(dev->net->dev_addr, mac, ETH_ALEN);
	else {
		printk(KERN_WARNING
			"dm9601: No valid MAC address in EEPROM, using %pM\n",
			dev->net->dev_addr);
		__dm9601_set_mac_address(dev);
	}*/

	return 1;
}
