/****************************************************************************
 * drivers/net/ftmac100.c
 * Faraday FTMAC100 Ethernet MAC Driver
 *
 *   Copyright (C) 2015 Anton D. Kachalov. All rights reserved.
 *   Author: Anton D. Kachalov <mouse@yandex.ru>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 3. Neither the name NuttX nor the names of its contributors may be
 *    used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#if defined(CONFIG_NET) && defined(CONFIG_NET_FTMAC100)

#include <stdint.h>
#include <stdlib.h>
#include <stdbool.h>
#include <time.h>
#include <string.h>
#include <errno.h>
#include <assert.h>
#include <debug.h>

#include <arpa/inet.h>

#include <nuttx/arch.h>
#include <nuttx/irq.h>
#include <nuttx/wdog.h>

#ifdef CONFIG_NET_NOINTS
#  include <nuttx/wqueue.h>
#endif

#include <nuttx/net/arp.h>
#include <nuttx/net/netdev.h>
#include <nuttx/net/ftmac100.h>

#ifdef CONFIG_NET_PKT
#  include <nuttx/net/pkt.h>
#endif

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/
/* If processing is not done at the interrupt level, then high priority
 * work queue support is required.
 */

#if defined(CONFIG_NET_NOINTS) && !defined(CONFIG_SCHED_HPWORK)
#  error High priority work queue support is required
#endif

/* CONFIG_FTMAC100_NINTERFACES determines the number of physical interfaces
 * that will be supported.
 */

#ifndef CONFIG_FTMAC100_NINTERFACES
# define CONFIG_FTMAC100_NINTERFACES 1
#endif

/* TX poll delay = 1 seconds. CLK_TCK is the number of clock ticks per second */

#define FTMAC100_WDDELAY   (1*CLK_TCK)
#define FTMAC100_POLLHSEC  (1*2)

/* TX timeout = 1 minute */

#define FTMAC100_TXTIMEOUT (60*CLK_TCK)

/* This is a helper pointer for accessing the contents of the Ethernet header */

#define BUF ((struct eth_hdr_s *)priv->ft_dev.d_buf)

/* RX/TX buffer alignment */

#define MAX_PKT_SIZE  1536
#define RX_BUF_SIZE   2044

#define ETH_ZLEN 60

#define MACCR_ENABLE_ALL (FTMAC100_MACCR_XMT_EN  | \
                          FTMAC100_MACCR_RCV_EN  | \
                          FTMAC100_MACCR_XDMA_EN | \
                          FTMAC100_MACCR_RDMA_EN | \
                          FTMAC100_MACCR_CRC_APD | \
                          FTMAC100_MACCR_FULLDUP | \
                          FTMAC100_MACCR_RX_RUNT | \
                          FTMAC100_MACCR_RX_BROADPKT)

#define MACCR_DISABLE_ALL 0

#define INT_MASK_ALL_ENABLED (FTMAC100_INT_RPKT_FINISH | \
                          FTMAC100_INT_NORXBUF | \
                          FTMAC100_INT_XPKT_OK | \
                          FTMAC100_INT_XPKT_LOST | \
                          FTMAC100_INT_RPKT_LOST | \
                          FTMAC100_INT_AHB_ERR | \
                          FTMAC100_INT_PHYSTS_CHG)

#define INT_MASK_ALL_DISABLED 0

#define putreg32(v, x) (*(volatile uint32_t*)(x) = (v))
#define getreg32(x) (*(uint32_t *)(x))

/****************************************************************************
 * Private Types
 ****************************************************************************/

/* The ftmac100_driver_s encapsulates all state information for a single hardware
 * interface
 */

struct ftmac100_driver_s
{
  struct ftmac100_txdes_s txdes[CONFIG_FTMAC100_TX_DESC];
  struct ftmac100_rxdes_s rxdes[CONFIG_FTMAC100_RX_DESC];
  int rx_pointer;
  int tx_pointer;
  int tx_clean_pointer;
  int tx_pending;
  uint32_t iobase;

  /* NuttX net data */
  bool ft_bifup;               /* true:ifup false:ifdown */
  WDOG_ID ft_txpoll;           /* TX poll timer */
  WDOG_ID ft_txtimeout;        /* TX timeout timer */
#ifdef CONFIG_NET_NOINTS
  unsigned int status;         /* Last ISR status */
  struct work_s ft_work;       /* For deferring work to the work queue */
#endif

  /* This holds the information visible to uIP/NuttX */

  struct net_driver_s ft_dev;  /* Interface understood by uIP */
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

static struct ftmac100_driver_s g_ftmac100[CONFIG_FTMAC100_NINTERFACES]
  __attribute__((aligned(16)));

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

/* FIXME: import low-level functions for IRQ setup */

extern inline void ftintc010_set_trig_mode(int irq, int mode);
extern inline void ftintc010_set_trig_level(int irq, int level);
extern inline void ftintc010_unmask_irq(int irq);
extern inline void ftintc010_mask_irq(int irq);

/* Common TX logic */

static int  ftmac100_transmit(FAR struct ftmac100_driver_s *priv);
static int  ftmac100_txpoll(struct net_driver_s *dev);

/* Interrupt handling */

static void ftmac100_reset(FAR struct ftmac100_driver_s *priv);
static void ftmac100_receive(FAR struct ftmac100_driver_s *priv);
static void ftmac100_txdone(FAR struct ftmac100_driver_s *priv);
static inline void ftmac100_interrupt_process(FAR struct ftmac100_driver_s *priv);
#ifdef CONFIG_NET_NOINTS
static void ftmac100_interrupt_work(FAR void *arg);
#endif
static int  ftmac100_interrupt(int irq, FAR void *context);

/* Watchdog timer expirations */

static inline void ftmac100_txtimeout_process(FAR struct ftmac100_driver_s *priv);
#ifdef CONFIG_NET_NOINTS
static void ftmac100_txtimeout_work(FAR void *arg);
#endif
static void ftmac100_txtimeout_expiry(int argc, uint32_t arg, ...);

static inline void ftmac100_poll_process(FAR struct ftmac100_driver_s *priv);
#ifdef CONFIG_NET_NOINTS
static void ftmac100_poll_work(FAR void *arg);
#endif
static void ftmac100_poll_expiry(int argc, uint32_t arg, ...);

/* NuttX callback functions */

static int ftmac100_ifup(FAR struct net_driver_s *dev);
static int ftmac100_ifdown(FAR struct net_driver_s *dev);
static inline void ftmac100_txavail_process(FAR struct ftmac100_driver_s *priv);
#ifdef CONFIG_NET_NOINTS
static void ftmac100_txavail_work(FAR void *arg);
#endif
static int ftmac100_txavail(FAR struct net_driver_s *dev);
#if defined(CONFIG_NET_IGMP) || defined(CONFIG_NET_ICMPv6)
static int ftmac100_addmac(FAR struct net_driver_s *dev, FAR const uint8_t *mac);
#ifdef CONFIG_NET_IGMP
static int ftmac100_rmmac(FAR struct net_driver_s *dev, FAR const uint8_t *mac);
#endif
#ifdef CONFIG_NET_ICMPv6
static void ftmac100_ipv6multicast(FAR struct ftmac100_driver_s *priv);
#endif
#endif

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Function: ftmac100_transmit
 *
 * Description:
 *   Start hardware transmission.  Called either from the txdone interrupt
 *   handling or from watchdog based polling.
 *
 * Parameters:
 *   priv - Reference to the driver state structure
 *
 * Returned Value:
 *   OK on success; a negated errno on failure
 *
 * Assumptions:
 *   May or may not be called from an interrupt handler.  In either case,
 *   global interrupts are disabled, either explicitly or indirectly through
 *   interrupt handling logic.
 *
 ****************************************************************************/

static FAR struct ftmac100_rxdes_s *
ftmac100_current_rxdes(FAR struct ftmac100_driver_s *priv)
{
  return &priv->rxdes[priv->rx_pointer];
}

static FAR struct ftmac100_txdes_s *
ftmac100_current_txdes(FAR struct ftmac100_driver_s *priv)
{
  return &priv->txdes[priv->tx_pointer];
}

static FAR struct ftmac100_txdes_s *
ftmac100_current_clean_txdes(FAR struct ftmac100_driver_s *priv)
{
  return &priv->txdes[priv->tx_clean_pointer];
}

static int ftmac100_transmit(FAR struct ftmac100_driver_s *priv)
{
  FAR struct ftmac100_register_s *iobase = (FAR struct ftmac100_register_s *)priv->iobase;
  FAR struct ftmac100_txdes_s *txdes;
  int len = priv->ft_dev.d_len;
//  irqstate_t flags;
//  flags = irqsave();
//  nvdbg("flags=%08x\n", flags);

  txdes = ftmac100_current_txdes(priv);

  /* Verify that the hardware is ready to send another packet.  If we get
   * here, then we are committed to sending a packet; Higher level logic
   * must have assured that there is no transmission in progress.
   */

  /* Increment statistics */

  len = len < ETH_ZLEN ? ETH_ZLEN : len;

  /* Send the packet: address=priv->ft_dev.d_buf, length=priv->ft_dev.d_len */

//  memcpy((void *)txdes->txdes2, priv->ft_dev.d_buf, len);
  txdes->txdes2  = (unsigned int)priv->ft_dev.d_buf;
  txdes->txdes1 &= FTMAC100_TXDES1_EDOTR;
  txdes->txdes1 |= (FTMAC100_TXDES1_FTS |
                    FTMAC100_TXDES1_LTS |
                    FTMAC100_TXDES1_TXIC |
                    FTMAC100_TXDES1_TXBUF_SIZE(len));
  txdes->txdes0 |= FTMAC100_TXDES0_TXDMA_OWN;

  nvdbg("ftmac100_transmit[%x]: copy %08x to %08x %04x\n",
        priv->tx_pointer, priv->ft_dev.d_buf, txdes->txdes2, len);

  priv->tx_pointer = (priv->tx_pointer + 1) & (CONFIG_FTMAC100_TX_DESC - 1);
  priv->tx_pending++;

  /* Enable Tx polling */
  // FIXME: enable interrupts
  putreg32(1, &iobase->txpd);

  /* Setup the TX timeout watchdog (perhaps restarting the timer) */

  (void)wd_start(priv->ft_txtimeout, FTMAC100_TXTIMEOUT,
                 ftmac100_txtimeout_expiry, 1, (uint32_t)priv);

//  irqrestore(flags);
  return OK;
}

/****************************************************************************
 * Function: ftmac100_txpoll
 *
 * Description:
 *   The transmitter is available, check if uIP has any outgoing packets
 *   ready to send.  This is a callback from devif_poll().  devif_poll() may
 *   be called:
 *
 *   1. When the preceding TX packet send is complete,
 *   2. When the preceding TX packet send timesout and the interface is
 *      reset
 *   3. During normal TX polling
 *
 * Parameters:
 *   dev - Reference to the NuttX driver state structure
 *
 * Returned Value:
 *   OK on success; a negated errno on failure
 *
 * Assumptions:
 *   May or may not be called from an interrupt handler.  In either case,
 *   global interrupts are disabled, either explicitly or indirectly through
 *   interrupt handling logic.
 *
 ****************************************************************************/

static int ftmac100_txpoll(struct net_driver_s *dev)
{
  FAR struct ftmac100_driver_s *priv = (FAR struct ftmac100_driver_s *)dev->d_private;

  /* If the polling resulted in data that should be sent out on the network,
   * the field d_len is set to a value > 0.
   */

  if (priv->ft_dev.d_len > 0)
    {
      /* Look up the destination MAC address and add it to the Ethernet
       * header.
       */

#ifdef CONFIG_NET_IPv4
#ifdef CONFIG_NET_IPv6
      if (IFF_IS_IPv4(priv->ft_dev.d_flags))
#endif
        {
          arp_out(&priv->ft_dev);
        }
#endif /* CONFIG_NET_IPv4 */

#ifdef CONFIG_NET_IPv6
#ifdef CONFIG_NET_IPv4
      else
#endif
        {
          neighbor_out(&priv->ft_dev);
        }
#endif /* CONFIG_NET_IPv6 */

      /* Send the packet */

      ftmac100_transmit(priv);

      /* Check if there is room in the device to hold another packet. If not,
       * return a non-zero value to terminate the poll.
       */
    }

  /* If zero is returned, the polling will continue until all connections have
   * been examined.
   */

  return 0;
}

/****************************************************************************
 * Function: ftmac100_reset
 *
 * Description:
 *   Do the HW reset
 *
 * Parameters:
 *   priv - Reference to the NuttX driver state structure
 *
 * Returned Value:
 *   None
 *
 * Assumptions:
 *   Global interrupts are disabled by interrupt handling logic.
 *
 ****************************************************************************/

static void ftmac100_reset(FAR struct ftmac100_driver_s *priv)
{
  FAR struct ftmac100_register_s *iobase = (FAR struct ftmac100_register_s *)priv->iobase;

  nvdbg("%s(): iobase=%p\n", __func__, iobase);

  putreg32 (FTMAC100_MACCR_SW_RST, &iobase->maccr);

  while (getreg32 (&iobase->maccr) & FTMAC100_MACCR_SW_RST)
    ;
}

/****************************************************************************
 * Function: ftmac100_init
 *
 * Description:
 *   Perform HW initialization
 *
 * Parameters:
 *   priv - Reference to the NuttX driver state structure
 *
 * Returned Value:
 *   None
 *
 * Assumptions:
 *   Global interrupts are disabled by interrupt handling logic.
 *
 ****************************************************************************/

static void ftmac100_init(FAR struct ftmac100_driver_s *priv)
{
  FAR struct ftmac100_register_s *iobase = (FAR struct ftmac100_register_s *)priv->iobase;
  FAR struct ftmac100_txdes_s *txdes = priv->txdes;
  FAR struct ftmac100_rxdes_s *rxdes = priv->rxdes;
  FAR unsigned char *kmem;
  int i;

  ndbg ("%s()\n", __func__);

  /* Disable all interrupts */

  putreg32(0, &iobase->imr);

  /* Initialize descriptors */

  priv->rx_pointer = 0;
  priv->tx_pointer = 0;
  priv->tx_clean_pointer = 0;
  priv->tx_pending = 0;

  rxdes[CONFIG_FTMAC100_RX_DESC - 1].rxdes1 = FTMAC100_RXDES1_EDORR;

  kmem = memalign(RX_BUF_SIZE, CONFIG_FTMAC100_RX_DESC * RX_BUF_SIZE);

  nvdbg("KMEM=%08x\n", kmem);

  for (i = 0; i < CONFIG_FTMAC100_RX_DESC; i++)
    {
      /* RXBUF_BADR */

      rxdes[i].rxdes0 = FTMAC100_RXDES0_RXDMA_OWN;
      rxdes[i].rxdes1 |= FTMAC100_RXDES1_RXBUF_SIZE(RX_BUF_SIZE);
      rxdes[i].rxdes2 = (unsigned int)(kmem + i * RX_BUF_SIZE);
      rxdes[i].rxdes3 = (unsigned int)(rxdes + i + 1); // next ring
    }

  rxdes[CONFIG_FTMAC100_RX_DESC - 1].rxdes3 = (unsigned int)rxdes; // next ring

  for (i = 0; i < CONFIG_FTMAC100_TX_DESC; i++)
    {
      /* TXBUF_BADR */

      txdes[i].txdes0 = 0;
      txdes[i].txdes1 = 0;
      txdes[i].txdes2 = 0;
      txdes[i].txdes3 = 0;
//    txdes[i].txdes3 = (unsigned int)(txdes + i + 1); // next ring
    }

  txdes[CONFIG_FTMAC100_TX_DESC - 1].txdes1 = FTMAC100_TXDES1_EDOTR;
//  txdes[CONFIG_FTMAC100_TX_DESC - 1].txdes3 = (unsigned int)txdes; // next ring

  /* transmit ring */

  nvdbg("priv=%08x txdes=%08x rxdes=%08x\n", priv, txdes, rxdes);

  putreg32 ((unsigned int)txdes, &iobase->txr_badr);

  /* receive ring */

  putreg32 ((unsigned int)rxdes, &iobase->rxr_badr);

  /* set RXINT_THR and TXINT_THR */

//  putreg32 (FTMAC100_ITC_RXINT_THR(1) | FTMAC100_ITC_TXINT_THR(1), &iobase->itc);

  /* poll receive descriptor automatically */

  putreg32 (FTMAC100_APTC_RXPOLL_CNT(1), &iobase->aptc);

#if 1
  /* Set DMA burst length */

  putreg32 (FTMAC100_DBLAC_RXFIFO_LTHR(2) |
            FTMAC100_DBLAC_RXFIFO_HTHR(6) |
            FTMAC100_DBLAC_RX_THR_EN, &iobase->dblac);

//  putreg32 (getreg32(&iobase->fcr) | 0x1, &iobase->fcr);
//  putreg32 (getreg32(&iobase->bpr) | 0x1, &iobase->bpr);
#endif

  /* enable transmitter, receiver */

  putreg32 (MACCR_ENABLE_ALL, &iobase->maccr);

  /* enable Rx, Tx interrupts */

  putreg32 (INT_MASK_ALL_ENABLED, &iobase->imr);
}

/****************************************************************************
 * Function: ftmac100_mdio_read
 *
 * Description:
 *   Read MII registers
 *
 * Parameters:
 *   iobase - Pointer to the driver's registers base
 *      reg - MII register number
 *
 * Returned Value:
 *   Register value
 *
 * Assumptions:
 *
 *
 ****************************************************************************/

static uint32_t ftmac100_mdio_read(FAR struct ftmac100_register_s *iobase, int reg)
{
  int i;
  uint32_t phycr = FTMAC100_PHYCR_PHYAD(1)   |
                   FTMAC100_PHYCR_REGAD(reg) |
                   FTMAC100_PHYCR_MIIRD;

  putreg32(phycr, &iobase->phycr);

  for (i = 0; i < 10; i++)
    {
      phycr = getreg32(&iobase->phycr);
      nvdbg("%02x %d phycr=%08x\n", reg, i, phycr);

      if ((phycr & FTMAC100_PHYCR_MIIRD) == 0)
        {
          break;
        }
    }

  return phycr & 0xffff;
}

/****************************************************************************
 * Function: ftmac100_set_mac
 *
 * Description:
 *   Set the MAC address
 *
 * Parameters:
 *   priv - Reference to the NuttX driver state structure
 *    mac - Six bytes MAC address
 *
 * Returned Value:
 *   None
 *
 * Assumptions:
 *
 ****************************************************************************/

static void ftmac100_set_mac(FAR struct ftmac100_driver_s *priv,
                             FAR const unsigned char *mac)
{
  FAR struct ftmac100_register_s *iobase = (FAR struct ftmac100_register_s *)priv->iobase;
  unsigned int maddr = mac[0] << 8 | mac[1];
  unsigned int laddr = mac[2] << 24 | mac[3] << 16 | mac[4] << 8 | mac[5];

  nvdbg("%s(%x %x)\n", __func__, maddr, laddr);

  putreg32(maddr, &iobase->mac_madr);
  putreg32(laddr, &iobase->mac_ladr);
}

/****************************************************************************
 * Function: ftmac100_receive
 *
 * Description:
 *   An interrupt was received indicating the availability of a new RX packet
 *
 * Parameters:
 *   priv - Reference to the driver state structure
 *
 * Returned Value:
 *   None
 *
 * Assumptions:
 *   Global interrupts are disabled by interrupt handling logic.
 *
 ****************************************************************************/

static void ftmac100_receive(FAR struct ftmac100_driver_s *priv)
{
  FAR struct ftmac100_rxdes_s *rxdes;
  FAR uint8_t *data;
  uint32_t len;

  rxdes = ftmac100_current_rxdes(priv);

  while (!(rxdes->rxdes0 & FTMAC100_RXDES0_RXDMA_OWN))
    {
      if (rxdes->rxdes0 & FTMAC100_RXDES0_FRS)
        goto found_segment;

      /* Clear status bits */

      rxdes->rxdes0 = FTMAC100_RXDES0_RXDMA_OWN;

      priv->rx_pointer = (priv->rx_pointer + 1) & (CONFIG_FTMAC100_RX_DESC - 1);
      rxdes = ftmac100_current_rxdes(priv);
    }

  ndbg("\nNOT FOUND\nCurrent RX %d rxdes0=%08x\n",
       priv->rx_pointer, rxdes->rxdes0);

  return;

found_segment:

  len = FTMAC100_RXDES0_RFL(rxdes->rxdes0);
  data = (uint8_t *)rxdes->rxdes2;

  ndbg ("RX buffer %d (%08x), %x received (%d)\n",
        priv->rx_pointer, data, len, (rxdes->rxdes0 & FTMAC100_RXDES0_LRS));

  do
    {
      /* Check for errors and update statistics */

      /* Check if the packet is a valid size for the uIP buffer configuration */

      /* Copy the data data from the hardware to priv->ft_dev.d_buf.  Set
       * amount of data in priv->ft_dev.d_len
       */

      memcpy(priv->ft_dev.d_buf, data, len);
      priv->ft_dev.d_len = len;

#ifdef CONFIG_NET_PKT
      /* When packet sockets are enabled, feed the frame into the packet tap */

       pkt_input(&priv->ft_dev);
#endif

      /* We only accept IP packets of the configured type and ARP packets */

#ifdef CONFIG_NET_IPv4
      if (BUF->type == HTONS(ETHTYPE_IP))
        {
          nllvdbg("IPv4 frame\n");

          /* Handle ARP on input then give the IPv4 packet to the network
           * layer
           */

          arp_ipin(&priv->ft_dev);
          ipv4_input(&priv->ft_dev);

          /* If the above function invocation resulted in data that should be
           * sent out on the network, the field  d_len will set to a value > 0.
           */

          if (priv->ft_dev.d_len > 0)
            {
              /* Update the Ethernet header with the correct MAC address */

#ifdef CONFIG_NET_IPv6
              if (IFF_IS_IPv4(priv->ft_dev.d_flags))
#endif
                {
                  arp_out(&priv->ft_dev);
                }
#ifdef CONFIG_NET_IPv6
              else
                {
                  neighbor_out(&priv->ft_dev);
                }
#endif

              /* And send the packet */

              ftmac100_transmit(priv);
            }
        }
      else
#endif
#ifdef CONFIG_NET_IPv6
      if (BUF->type == HTONS(ETHTYPE_IP6))
        {
          nllvdbg("Iv6 frame\n");

          /* Give the IPv6 packet to the network layer */

          ipv6_input(&priv->ft_dev);

          /* If the above function invocation resulted in data that should be
           * sent out on the network, the field  d_len will set to a value > 0.
           */

          if (priv->ft_dev.d_len > 0)
           {
              /* Update the Ethernet header with the correct MAC address */

#ifdef CONFIG_NET_IPv4
              if (IFF_IS_IPv4(priv->ft_dev.d_flags))
                {
                  arp_out(&priv->ft_dev);
                }
              else
#endif
#ifdef CONFIG_NET_IPv6
                {
                  neighbor_out(&priv->ft_dev);
                }
#endif

              /* And send the packet */

              ftmac100_transmit(priv);
            }
        }
      else
#endif
#ifdef CONFIG_NET_ARP
      if (BUF->type == htons(ETHTYPE_ARP))
        {
          arp_arpin(&priv->ft_dev);

          /* If the above function invocation resulted in data that should be
           * sent out on the network, the field  d_len will set to a value > 0.
           */

          if (priv->ft_dev.d_len > 0)
            {
              ftmac100_transmit(priv);
            }
        }
#endif
    }
  while (0); /* While there are more packets to be processed */

  priv->rx_pointer = (priv->rx_pointer + 1) & (CONFIG_FTMAC100_RX_DESC - 1);

  rxdes->rxdes1 &= FTMAC100_RXDES1_EDORR;
  rxdes->rxdes1 |= FTMAC100_RXDES1_RXBUF_SIZE(RX_BUF_SIZE);
  rxdes->rxdes0 |= FTMAC100_RXDES0_RXDMA_OWN;
}

/****************************************************************************
 * Function: ftmac100_txdone
 *
 * Description:
 *   An interrupt was received indicating that the last TX packet(s) is done
 *
 * Parameters:
 *   priv - Reference to the driver state structure
 *
 * Returned Value:
 *   None
 *
 * Assumptions:
 *   Global interrupts are disabled by the watchdog logic.
 *
 ****************************************************************************/

static void ftmac100_txdone(FAR struct ftmac100_driver_s *priv)
{
  FAR struct ftmac100_txdes_s *txdes;

  /* Check for errors and update statistics */

  while (priv->tx_pending)
    {
      txdes = ftmac100_current_clean_txdes(priv);

      /* txdes owned by dma */

      if (txdes->txdes0 & FTMAC100_TXDES0_TXDMA_OWN)
        break;

      /* TODO: check for excessive and late collisions */

      /* txdes reset */

      txdes->txdes0 = 0;
      txdes->txdes1 &= FTMAC100_TXDES1_EDOTR;
      txdes->txdes2 = 0;
      txdes->txdes3 = 0;

      priv->tx_clean_pointer = (priv->tx_clean_pointer + 1) & (CONFIG_FTMAC100_TX_DESC - 1);

      priv->tx_pending--;
    }

  /* If no further xmits are pending, then cancel the TX timeout and
   * disable further Tx interrupts.
   */

  nvdbg("txpending=%d\n", priv->tx_pending);

  wd_cancel(priv->ft_txtimeout);

  /* Then poll uIP for new XMIT data */

  (void)devif_poll(&priv->ft_dev, ftmac100_txpoll);
}

/****************************************************************************
 * Function: ftmac100_interrupt_process
 *
 * Description:
 *   Interrupt processing.  This may be performed either within the interrupt
 *   handler or on the worker thread, depending upon the configuration
 *
 * Parameters:
 *   priv - Reference to the driver state structure
 *
 * Returned Value:
 *   None
 *
 * Assumptions:
 *   Ethernet interrupts are disabled
 *
 ****************************************************************************/

static inline void ftmac100_interrupt_process(FAR struct ftmac100_driver_s *priv)
{
  FAR struct ftmac100_register_s *iobase = (FAR struct ftmac100_register_s *)priv->iobase;
  unsigned int status;
  unsigned int phycr;

#ifdef CONFIG_NET_NOINTS
  status = priv->status;
#else
  status = getreg32 (&iobase->isr);
#endif

  nvdbg("status=%08x(%08x) BASE=%p ISR=%p PHYCR=%p\n",
        status, getreg32(&iobase->isr), iobase, &iobase->isr, &iobase->phycr);

  if (!status)
    {
      goto out;
    }

  /* Handle interrupts according to status bit settings */

  /* Check if we received an incoming packet, if so, call ftmac100_receive() */

  if (status & FTMAC100_INT_RPKT_SAV)
    {
      putreg32(1, &iobase->rxpd);
    }

  if (status & (FTMAC100_INT_RPKT_FINISH | FTMAC100_INT_NORXBUF))
    {
      ftmac100_receive(priv);
    }

  /* Check if a packet transmission just completed.  If so, call ftmac100_txdone.
   * This may disable further Tx interrupts if there are no pending
   * transmissions.
   */

  if (status & (FTMAC100_INT_XPKT_OK))
    {
      nvdbg("\n\nTXDONE\n\n");
      ftmac100_txdone(priv);
    }

  if (status & FTMAC100_INT_PHYSTS_CHG)
    {
      /* PHY link status change */

      phycr = ftmac100_mdio_read(iobase, 1);
      if (phycr & 0x04)
        {
          priv->ft_bifup = true;
        }
      else
        {
          priv->ft_bifup = false;
        }

      nvdbg("Link: %s\n", priv->ft_bifup ? "UP" : "DOWN");
      ftmac100_mdio_read(iobase, 5);
    }

#if 0
#define REG(x) (*(volatile uint32_t *)(x))
  ndbg("\n=============================================================\n");
  ndbg("TM CNTL=%08x INTRS=%08x MASK=%08x LOAD=%08x COUNT=%08x M1=%08x\n",
       REG(0x98400030), REG(0x98400034), REG(0x98400038), REG(0x98400004),
       REG(0x98400000), REG(0x98400008));
  ndbg("IRQ STATUS=%08x MASK=%08x MODE=%08x LEVEL=%08x\n",
       REG(0x98800014), REG(0x98800004), REG(0x9880000C), REG(0x98800010));
  ndbg("FIQ STATUS=%08x MASK=%08x MODE=%08x LEVEL=%08x\n", REG(0x98800034),
       REG(0x98800024), REG(0x9880002C), REG(0x98800020));
  ndbg("=============================================================\n");
#endif

out:
  putreg32 (INT_MASK_ALL_ENABLED, &iobase->imr);

  ndbg("ISR-done\n");
}

/****************************************************************************
 * Function: ftmac100_interrupt_work
 *
 * Description:
 *   Perform interrupt related work from the worker thread
 *
 * Parameters:
 *   arg - The argument passed when work_queue() was called.
 *
 * Returned Value:
 *   OK on success
 *
 * Assumptions:
 *   Ethernet interrupts are disabled
 *
 ****************************************************************************/

#ifdef CONFIG_NET_NOINTS
static void ftmac100_interrupt_work(FAR void *arg)
{
  FAR struct ftmac100_driver_s *priv = (FAR struct ftmac100_driver_s *)arg;
  net_lock_t state;
//  irqstate_t flags;

  /* Process pending Ethernet interrupts */

  state = net_lock();
//  flags = irqsave();

  ftmac100_interrupt_process(priv);

//  irqrestore(flags);
  net_unlock(state);

  /* Re-enable Ethernet interrupts */

//  up_enable_irq(CONFIG_FTMAC100_IRQ);
  ftintc010_unmask_irq(CONFIG_FTMAC100_IRQ);
//  ftintc010_set_trig_mode(CONFIG_FTMAC100_IRQ, 0);
//  ftintc010_set_trig_level(CONFIG_FTMAC100_IRQ, 0);
}
#endif

/****************************************************************************
 * Function: ftmac100_interrupt
 *
 * Description:
 *   Hardware interrupt handler
 *
 * Parameters:
 *   irq     - Number of the IRQ that generated the interrupt
 *   context - Interrupt register state save info (architecture-specific)
 *
 * Returned Value:
 *   OK on success
 *
 * Assumptions:
 *
 ****************************************************************************/

static int ftmac100_interrupt(int irq, FAR void *context)
{
  FAR struct ftmac100_driver_s *priv = &g_ftmac100[0];
  FAR struct ftmac100_register_s *iobase = (FAR struct ftmac100_register_s *)priv->iobase;

#ifdef CONFIG_NET_NOINTS
  irqstate_t flags;

  /* Disable further Ethernet interrupts.  Because Ethernet interrupts are
   * also disabled if the TX timeout event occurs, there can be no race
   * condition here.
   */

  flags = irqsave();

  priv->status = getreg32 (&iobase->isr);

//  up_disable_irq(CONFIG_FTMAC100_IRQ);
  ftintc010_mask_irq(CONFIG_FTMAC100_IRQ);
//  ftintc010_set_trig_mode(CONFIG_FTMAC100_IRQ, 1);
//  ftintc010_set_trig_level(CONFIG_FTMAC100_IRQ, 1);

  putreg32 (INT_MASK_ALL_DISABLED, &iobase->imr);

  /* TODO: Determine if a TX transfer just completed */

  nvdbg("===> status=%08x\n", priv->status);

  if (priv->status & (FTMAC100_INT_XPKT_OK))
    {
      /* If a TX transfer just completed, then cancel the TX timeout so
       * there will be do race condition between any subsequent timeout
       * expiration and the deferred interrupt processing.
       */

      nvdbg("\n\nTXDONE 0\n\n");
      wd_cancel(priv->ft_txtimeout);
    }

  /* Cancel any pending poll work */

  work_cancel(HPWORK, &priv->ft_work);

  /* Schedule to perform the interrupt processing on the worker thread. */

  work_queue(HPWORK, &priv->ft_work, ftmac100_interrupt_work, priv, 0);

  irqrestore(flags);
#else
  /* Process the interrupt now */
  putreg32 (INT_MASK_ALL_DISABLED, &iobase->imr);

  ftmac100_interrupt_process(priv);
#endif

  return OK;
}

/****************************************************************************
 * Function: ftmac100_txtimeout_process
 *
 * Description:
 *   Process a TX timeout.  Called from the either the watchdog timer
 *   expiration logic or from the worker thread, depending upon the
 *   configuration.  The timeout means that the last TX never completed.
 *   Reset the hardware and start again.
 *
 * Parameters:
 *   priv - Reference to the driver state structure
 *
 * Returned Value:
 *   None
 *
 ****************************************************************************/

static inline void ftmac100_txtimeout_process(FAR struct ftmac100_driver_s *priv)
{
  /* Increment statistics and dump debug info */

  /* Then reset the hardware */

  nvdbg("TXTIMEOUT\n");

  /* Then poll uIP for new XMIT data */

  (void)devif_poll(&priv->ft_dev, ftmac100_txpoll);
}

/****************************************************************************
 * Function: ftmac100_txtimeout_work
 *
 * Description:
 *   Perform TX timeout related work from the worker thread
 *
 * Parameters:
 *   arg - The argument passed when work_queue() as called.
 *
 * Returned Value:
 *   OK on success
 *
 * Assumptions:
 *   Ethernet interrupts are disabled
 *
 ****************************************************************************/

#ifdef CONFIG_NET_NOINTS
static void ftmac100_txtimeout_work(FAR void *arg)
{
  FAR struct ftmac100_driver_s *priv = (FAR struct ftmac100_driver_s *)arg;
  net_lock_t state;

  /* Process pending Ethernet interrupts */

  state = net_lock();
  ftmac100_txtimeout_process(priv);
  net_unlock(state);
}
#endif

/****************************************************************************
 * Function: ftmac100_txtimeout_expiry
 *
 * Description:
 *   Our TX watchdog timed out.  Called from the timer interrupt handler.
 *   The last TX never completed.  Reset the hardware and start again.
 *
 * Parameters:
 *   argc - The number of available arguments
 *   arg  - The first argument
 *
 * Returned Value:
 *   None
 *
 * Assumptions:
 *   Global interrupts are disabled by the watchdog logic.
 *
 ****************************************************************************/

static void ftmac100_txtimeout_expiry(int argc, uint32_t arg, ...)
{
  FAR struct ftmac100_driver_s *priv = (FAR struct ftmac100_driver_s *)arg;

#ifdef CONFIG_NET_NOINTS
  /* Disable further Ethernet interrupts.  This will prevent some race
   * conditions with interrupt work.  There is still a potential race
   * condition with interrupt work that is already queued and in progress.
   */

//  up_disable_irq(CONFIG_FTMAC100_IRQ);
  ftintc010_mask_irq(CONFIG_FTMAC100_IRQ);
//  ftintc010_set_trig_mode(CONFIG_FTMAC100_IRQ, 1);
//  ftintc010_set_trig_level(CONFIG_FTMAC100_IRQ, 1);

  /* Cancel any pending poll or interrupt work.  This will have no effect
   * on work that has already been started.
   */

  work_cancel(HPWORK, &priv->ft_work);

  /* Schedule to perform the TX timeout processing on the worker thread. */

  work_queue(HPWORK, &priv->ft_work, ftmac100_txtimeout_work, priv, 0);
#else
  /* Process the timeout now */

  ftmac100_txtimeout_process(priv);
#endif
}

/****************************************************************************
 * Function: ftmac100_poll_process
 *
 * Description:
 *   Perform the periodic poll.  This may be called either from watchdog
 *   timer logic or from the worker thread, depending upon the configuration.
 *
 * Parameters:
 *   priv - Reference to the driver state structure
 *
 * Returned Value:
 *   None
 *
 * Assumptions:
 *
 ****************************************************************************/

static inline void ftmac100_poll_process(FAR struct ftmac100_driver_s *priv)
{
  /* Check if there is room in the send another TX packet.  We cannot perform
   * the TX poll if he are unable to accept another packet for transmission.
   */

  /* If so, update TCP timing states and poll uIP for new XMIT data. Hmmm..
   * might be bug here.  Does this mean if there is a transmit in progress,
   * we will missing TCP time state updates?
   */

  (void)devif_timer(&priv->ft_dev, ftmac100_txpoll, FTMAC100_POLLHSEC);

  /* Setup the watchdog poll timer again */

  (void)wd_start(priv->ft_txpoll, FTMAC100_WDDELAY, ftmac100_poll_expiry, 1, priv);
}

/****************************************************************************
 * Function: ftmac100_poll_work
 *
 * Description:
 *   Perform periodic polling from the worker thread
 *
 * Parameters:
 *   arg - The argument passed when work_queue() as called.
 *
 * Returned Value:
 *   OK on success
 *
 * Assumptions:
 *   Ethernet interrupts are disabled
 *
 ****************************************************************************/

#ifdef CONFIG_NET_NOINTS
static void ftmac100_poll_work(FAR void *arg)
{
  FAR struct ftmac100_driver_s *priv = (FAR struct ftmac100_driver_s *)arg;
  net_lock_t state;

  /* Perform the poll */

  state = net_lock();
  ftmac100_poll_process(priv);
  net_unlock(state);
}
#endif

/****************************************************************************
 * Function: ftmac100_poll_expiry
 *
 * Description:
 *   Periodic timer handler.  Called from the timer interrupt handler.
 *
 * Parameters:
 *   argc - The number of available arguments
 *   arg  - The first argument
 *
 * Returned Value:
 *   None
 *
 * Assumptions:
 *   Global interrupts are disabled by the watchdog logic.
 *
 ****************************************************************************/

static void ftmac100_poll_expiry(int argc, uint32_t arg, ...)
{
  FAR struct ftmac100_driver_s *priv = (FAR struct ftmac100_driver_s *)arg;

#ifdef CONFIG_NET_NOINTS
  /* Is our single work structure available?  It may not be if there are
   * pending interrupt actions.
   */

  if (work_available(&priv->ft_work))
    {
      /* Schedule to perform the interrupt processing on the worker thread. */

      work_queue(HPWORK, &priv->ft_work, ftmac100_poll_work, priv, 0);
    }
  else
    {
      /* No.. Just re-start the watchdog poll timer, missing one polling
       * cycle.
       */

      (void)wd_start(priv->ft_txpoll, FTMAC100_WDDELAY, ftmac100_poll_expiry, 1, arg);
    }

#else
  /* Process the interrupt now */

  ftmac100_poll_process(priv);
#endif
}

/****************************************************************************
 * Function: ftmac100_ifup
 *
 * Description:
 *   NuttX Callback: Bring up the Ethernet interface when an IP address is
 *   provided
 *
 * Parameters:
 *   dev - Reference to the NuttX driver state structure
 *
 * Returned Value:
 *   None
 *
 * Assumptions:
 *
 ****************************************************************************/

static int ftmac100_ifup(struct net_driver_s *dev)
{
  FAR struct ftmac100_driver_s *priv = (FAR struct ftmac100_driver_s *)dev->d_private;

#ifdef CONFIG_NET_IPv4
  ndbg("Bringing up: %d.%d.%d.%d\n",
       dev->d_ipaddr & 0xff, (dev->d_ipaddr >> 8) & 0xff,
       (dev->d_ipaddr >> 16) & 0xff, dev->d_ipaddr >> 24);
#endif
#ifdef CONFIG_NET_IPv6
  ndbg("Bringing up: %04x:%04x:%04x:%04x:%04x:%04x:%04x:%04x\n",
       dev->d_ipv6addr[0], dev->d_ipv6addr[1], dev->d_ipv6addr[2],
       dev->d_ipv6addr[3], dev->d_ipv6addr[4], dev->d_ipv6addr[5],
       dev->d_ipv6addr[6], dev->d_ipv6addr[7]);
#endif

  /* Initialize PHYs, the Ethernet interface, and setup up Ethernet
   * interrupts.
   */

  ftmac100_init(priv);

  /* Instantiate the MAC address from priv->ft_dev.d_mac.ether_addr_octet */

  ftmac100_set_mac(priv, priv->ft_dev.d_mac.ether_addr_octet);

#ifdef CONFIG_NET_ICMPv6
  /* Set up IPv6 multicast address filtering */

  ftmac100_ipv6multicast(priv);
#endif

  /* Set and activate a timer process */

  (void)wd_start(priv->ft_txpoll, FTMAC100_WDDELAY, ftmac100_poll_expiry, 1, (uint32_t)priv);

  /* Enable the Ethernet interrupt */

  priv->ft_bifup = true;
//  up_enable_irq(CONFIG_FTMAC100_IRQ);
  ftintc010_unmask_irq(CONFIG_FTMAC100_IRQ);
  ftintc010_set_trig_mode(CONFIG_FTMAC100_IRQ, 0);
  ftintc010_set_trig_level(CONFIG_FTMAC100_IRQ, 0);
  return OK;
}

/****************************************************************************
 * Function: ftmac100_ifdown
 *
 * Description:
 *   NuttX Callback: Stop the interface.
 *
 * Parameters:
 *   dev - Reference to the NuttX driver state structure
 *
 * Returned Value:
 *   None
 *
 * Assumptions:
 *
 ****************************************************************************/

static int ftmac100_ifdown(struct net_driver_s *dev)
{
  FAR struct ftmac100_driver_s *priv = (FAR struct ftmac100_driver_s *)dev->d_private;
  FAR struct ftmac100_register_s *iobase = (FAR struct ftmac100_register_s *)priv->iobase;
  irqstate_t flags;

  /* Disable the Ethernet interrupt */

  flags = irqsave();
//  up_disable_irq(CONFIG_FTMAC100_IRQ);
  ftintc010_mask_irq(CONFIG_FTMAC100_IRQ);
//  ftintc010_set_trig_mode(CONFIG_FTMAC100_IRQ, 1);
//  ftintc010_set_trig_level(CONFIG_FTMAC100_IRQ, 1);

  /* Cancel the TX poll timer and TX timeout timers */

  wd_cancel(priv->ft_txpoll);
  wd_cancel(priv->ft_txtimeout);

  /* Put the EMAC in its reset, non-operational state.  This should be
   * a known configuration that will guarantee the ftmac100_ifup() always
   * successfully brings the interface back up.
   */

  putreg32 (0, &iobase->maccr);

  /* Mark the device "down" */

  priv->ft_bifup = false;
  irqrestore(flags);
  return OK;
}

/****************************************************************************
 * Function: ftmac100_txavail_process
 *
 * Description:
 *   Perform an out-of-cycle poll.
 *
 * Parameters:
 *   dev - Reference to the NuttX driver state structure
 *
 * Returned Value:
 *   None
 *
 * Assumptions:
 *   Called in normal user mode
 *
 ****************************************************************************/

static inline void ftmac100_txavail_process(FAR struct ftmac100_driver_s *priv)
{
  /* Ignore the notification if the interface is not yet up */

  if (priv->ft_bifup)
    {
      /* Check if there is room in the hardware to hold another outgoing packet. */

      /* If so, then poll uIP for new XMIT data */

      (void)devif_poll(&priv->ft_dev, ftmac100_txpoll);
    }
}

/****************************************************************************
 * Function: ftmac100_txavail_work
 *
 * Description:
 *   Perform an out-of-cycle poll on the worker thread.
 *
 * Parameters:
 *   arg - Reference to the NuttX driver state structure (cast to void*)
 *
 * Returned Value:
 *   None
 *
 * Assumptions:
 *   Called on the higher priority worker thread.
 *
 ****************************************************************************/

#ifdef CONFIG_NET_NOINTS
static void ftmac100_txavail_work(FAR void *arg)
{
  FAR struct ftmac100_driver_s *priv = (FAR struct ftmac100_driver_s *)arg;
  net_lock_t state;

  /* Perform the poll */

  state = net_lock();
  ftmac100_txavail_process(priv);
  net_unlock(state);
}
#endif

/****************************************************************************
 * Function: ftmac100_txavail
 *
 * Description:
 *   Driver callback invoked when new TX data is available.  This is a
 *   stimulus perform an out-of-cycle poll and, thereby, reduce the TX
 *   latency.
 *
 * Parameters:
 *   dev - Reference to the NuttX driver state structure
 *
 * Returned Value:
 *   None
 *
 * Assumptions:
 *   Called in normal user mode
 *
 ****************************************************************************/

static int ftmac100_txavail(struct net_driver_s *dev)
{
  FAR struct ftmac100_driver_s *priv = (FAR struct ftmac100_driver_s *)dev->d_private;

#ifdef CONFIG_NET_NOINTS
  /* Is our single work structure available?  It may not be if there are
   * pending interrupt actions and we will have to ignore the Tx
   * availability action.
   */

  if (work_available(&priv->ft_work))
    {
      /* Schedule to serialize the poll on the worker thread. */

      work_queue(HPWORK, &priv->ft_work, ftmac100_txavail_work, priv, 0);
    }

#else
  irqstate_t flags;

  /* Disable interrupts because this function may be called from interrupt
   * level processing.
   */

  flags = irqsave();

  /* Perform the out-of-cycle poll now */

  ftmac100_txavail_process(priv);
  irqrestore(flags);
#endif

  return OK;
}

/****************************************************************************
 * Function: ftmac100_addmac
 *
 * Description:
 *   NuttX Callback: Add the specified MAC address to the hardware multicast
 *   address filtering
 *
 * Parameters:
 *   dev  - Reference to the NuttX driver state structure
 *   mac  - The MAC address to be added
 *
 * Returned Value:
 *   None
 *
 * Assumptions:
 *
 ****************************************************************************/

#if defined(CONFIG_NET_IGMP) || defined(CONFIG_NET_ICMPv6)
static int ftmac100_addmac(struct net_driver_s *dev, FAR const uint8_t *mac)
{
  FAR struct ftmac100_driver_s *priv = (FAR struct ftmac100_driver_s *)dev->d_private;

  /* Add the MAC address to the hardware multicast routing table */

  return OK;
}
#endif

/****************************************************************************
 * Function: ftmac100_rmmac
 *
 * Description:
 *   NuttX Callback: Remove the specified MAC address from the hardware multicast
 *   address filtering
 *
 * Parameters:
 *   dev  - Reference to the NuttX driver state structure
 *   mac  - The MAC address to be removed
 *
 * Returned Value:
 *   None
 *
 * Assumptions:
 *
 ****************************************************************************/

#ifdef CONFIG_NET_IGMP
static int ftmac100_rmmac(struct net_driver_s *dev, FAR const uint8_t *mac)
{
  FAR struct ftmac100_driver_s *priv = (FAR struct ftmac100_driver_s *)dev->d_private;

  /* Add the MAC address to the hardware multicast routing table */

  return OK;
}
#endif

/****************************************************************************
 * Function: ftmac100_ipv6multicast
 *
 * Description:
 *   Configure the IPv6 multicast MAC address.
 *
 * Parameters:
 *   priv - A reference to the private driver state structure
 *
 * Returned Value:
 *   OK on success; Negated errno on failure.
 *
 * Assumptions:
 *
 ****************************************************************************/

#ifdef CONFIG_NET_ICMPv6
static void ftmac100_ipv6multicast(FAR struct ftmac100_driver_s *priv)
{
  FAR struct net_driver_s *dev;
  uint16_t tmp16;
  uint8_t mac[6];

  /* For ICMPv6, we need to add the IPv6 multicast address
   *
   * For IPv6 multicast addresses, the Ethernet MAC is derived by
   * the four low-order octets OR'ed with the MAC 33:33:00:00:00:00,
   * so for example the IPv6 address FF02:DEAD:BEEF::1:3 would map
   * to the Ethernet MAC address 33:33:00:01:00:03.
   *
   * NOTES:  This appears correct for the ICMPv6 Router Solicitation
   * Message, but the ICMPv6 Neighbor Solicitation message seems to
   * use 33:33:ff:01:00:03.
   */

  mac[0] = 0x33;
  mac[1] = 0x33;

  dev    = &priv->ft_dev;
  tmp16  = dev->d_ipv6addr[6];
  mac[2] = 0xff;
  mac[3] = tmp16 >> 8;

  tmp16  = dev->d_ipv6addr[7];
  mac[4] = tmp16 & 0xff;
  mac[5] = tmp16 >> 8;

  nvdbg("IPv6 Multicast: %02x:%02x:%02x:%02x:%02x:%02x\n",
        mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

  (void)ftmac100_addmac(dev, mac);

#ifdef CONFIG_NET_ICMPv6_AUTOCONF
  /* Add the IPv6 all link-local nodes Ethernet address.  This is the
   * address that we expect to receive ICMPv6 Router Advertisement
   * packets.
   */

  (void)ftmac100_addmac(dev, g_ipv6_ethallnodes.ether_addr_octet);

#endif /* CONFIG_NET_ICMPv6_AUTOCONF */
#ifdef CONFIG_NET_ICMPv6_ROUTER
  /* Add the IPv6 all link-local routers Ethernet address.  This is the
   * address that we expect to receive ICMPv6 Router Solicitation
   * packets.
   */

  (void)ftmac100_addmac(dev, g_ipv6_ethallrouters.ether_addr_octet);

#endif /* CONFIG_NET_ICMPv6_ROUTER */
}
#endif /* CONFIG_NET_ICMPv6 */

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Function: ftmac100_initialize
 *
 * Description:
 *   Initialize the Ethernet controller and driver
 *
 * Parameters:
 *   intf - In the case where there are multiple EMACs, this value
 *          identifies which EMAC is to be initialized.
 *
 * Returned Value:
 *   OK on success; Negated errno on failure.
 *
 * Assumptions:
 *
 ****************************************************************************/

int ftmac100_initialize(int intf)
{
  struct ftmac100_driver_s *priv;

  /* Get the interface structure associated with this interface number. */

  DEBUGASSERT(intf < CONFIG_FTMAC100_NINTERFACES);
  priv = &g_ftmac100[intf];

  /* Attach the IRQ to the driver */

  if (irq_attach(CONFIG_FTMAC100_IRQ, ftmac100_interrupt))
    {
      /* We could not attach the ISR to the interrupt */

      return -EAGAIN;
    }

  /* Initialize the driver structure */

  memset(priv, 0, sizeof(struct ftmac100_driver_s));
  priv->ft_dev.d_ifup    = ftmac100_ifup;     /* I/F up (new IP address) callback */
  priv->ft_dev.d_ifdown  = ftmac100_ifdown;   /* I/F down callback */
  priv->ft_dev.d_txavail = ftmac100_txavail;  /* New TX data callback */
#ifdef CONFIG_NET_IGMP
  priv->ft_dev.d_addmac  = ftmac100_addmac;   /* Add multicast MAC address */
  priv->ft_dev.d_rmmac   = ftmac100_rmmac;    /* Remove multicast MAC address */
#endif
  priv->ft_dev.d_private = (void*)g_ftmac100; /* Used to recover private state from dev */

  /* Create a watchdog for timing polling for and timing of transmisstions */

  priv->ft_txpoll       = wd_create();   /* Create periodic poll timer */
  priv->ft_txtimeout    = wd_create();   /* Create TX timeout timer */

  priv->iobase          = CONFIG_FTMAC100_BASE;

  /* Put the interface in the down state.  This usually amounts to resetting
   * the device and/or calling ftmac100_ifdown().
   */
  ftmac100_reset(priv);

  /* Read the MAC address from the hardware into priv->ft_dev.d_mac.ether_addr_octet */

  memcpy(priv->ft_dev.d_mac.ether_addr_octet, (void *)(CONFIG_FTMAC100_MAC0_ENV_ADDR), 6);

  /* Register the device with the OS so that socket IOCTLs can be performed */

  (void)netdev_register(&priv->ft_dev, NET_LL_ETHERNET);
  return OK;
}

#endif /* CONFIG_NET && CONFIG_NET_FTMAC100 */