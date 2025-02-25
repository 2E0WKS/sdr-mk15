/*! \file ksz8851.c \brief Micrel KSZ8851SNL Ethernet Interface Driver. */
//*****************************************************************************
//
// File Name	: 'ksz8851snl.c'
// Title		: Micrel KSZ8851SNL Ethernet Interface Driver
// Author		: Ag Primatic (c)2009
// Created		: 1/17/2009
// Revised		: 1/17/2009
// Version		: 0.0
// Target MCU	: Atmel AVR series
// Editor Tabs	: 8
//
// Description	: This driver provides initialization and transmit/receive
//	functions for the Micrel KSZ8851SNL 10/100 Ethernet Controller and PHY.
// This chip is novel in that it is a full 10/100 MAC+PHY interface
// all in a 32-pin chip, using an SPI interface to the host processor.
//
//*****************************************************************************

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include "global-conf.h"
#include "ksz8851.h"

// ASF Stuff
#include "spi.h"
#include "spi_master.h"

// SPI Pin Configuration for AT32UC3B

#include "gpio.h"
#include "eth_spi.h"

static uint16_t	length_sum;
static uint8_t	frameID = 0;

#include "ksz8851conf.h"

#ifdef SPI_DMA
#include "pdca.h"
#endif

void delayms(uint16_t delms);		// borrowed from SDR_MK1.5.c, what uses ASF cpu_delay_ms(ms, F_CPU) function

/*
 SPI access helpers for AT32UC3B
*/

static inline void select_eth(void)
{
	spi_selectChip(ETH_SPI, ETH_SPI_NPCS);
}

static inline void unselect_eth(void)
{
	spi_unselectChip(ETH_SPI, 0);	// unselect all chipselects (the chip parameter is ignored by funtion anyway!). Also checks, if the TX buffer is empty before releasing chipselects
}
/*
static inline void spi_put8(volatile avr32_spi_t *spi, uint8_t data8)
{
uint16_t data16;

	data16=data8;
	while(!spi_is_tx_ready(spi)){};				// should not block (theoretically!)
	spi_put(spi, data16);
}

static inline uint8_t spi_get8(volatile avr32_spi_t *spi)
{
uint16_t data16;

	while (!spi_is_rx_ready(ETH_SPI)){};		// wait for the character to appear
	data16=spi_get(spi);
	return data16;
}
*/
//***********************

#ifdef SPI_DMA

volatile avr32_pdca_channel_t *pdca_channel_network_tx=NULL;


// PDCA channel options
static pdca_channel_options_t PDCA_OPTIONS_NETWORK_TX =
{
	.addr = 0, //(void *)dmabuff_1,						// memory address
	.pid = AVR32_PDCA_PID_SPI_TX,					// select peripheral
	.size = 0, //(DMAMASK+1),							// transfer counter
	.r_addr = 0, //(void *)dmabuff_1,					// next memory address (use the same, so we do have a ring buffer)
	.r_size = 0, //(DMAMASK+1),							// next transfer counter
	.transfer_size = PDCA_TRANSFER_SIZE_BYTE		// select size of the transfer
};

#ifdef DMA_RX
volatile avr32_pdca_channel_t *pdca_channel_network_rx=NULL;

static pdca_channel_options_t PDCA_OPTIONS_NETWORK_RX =
{
	.addr = 0, //(void *)dmabuff_1,						// memory address
	.pid = AVR32_PDCA_PID_SPI_RX,					// select peripheral
	.size = 0, //(DMAMASK+1),							// transfer counter
	.r_addr = 0, //(void *)dmabuff_1,					// next memory address (use the same, so we do have a ring buffer)
	.r_size = 0, //(DMAMASK+1),							// next transfer counter
	.transfer_size = PDCA_TRANSFER_SIZE_BYTE		// select size of the transfer
};
#endif

#endif

static void init_spi(void)
{
	// Basic SPI setup as far as pins and chipselects go is already done by SetupHardware() function at the SDR_MK1.5.c

#ifdef SPI_DMA
	// program DMA controller to transfer data to network chip. Note, that this is only working for transmission. For Rx part, oldschool
	// spi_op() transfers are used
	pdca_init_channel(PDCA_CHANNEL_1, &PDCA_OPTIONS_NETWORK_TX);				// init PDCA channel with options.
	pdca_channel_network_tx=pdca_get_handler(PDCA_CHANNEL_1);
	pdca_enable(PDCA_CHANNEL_1);
#ifdef DMA_RX
	pdca_init_channel(PDCA_CHANNEL_2, &PDCA_OPTIONS_NETWORK_RX);				// init PDCA channel with options.
	pdca_channel_network_rx=pdca_get_handler(PDCA_CHANNEL_2);
	pdca_enable(PDCA_CHANNEL_2);
#endif
#endif

}

//uint8_t xdata;

/* spi_byte() sends one byte (outdat) and returns the received byte */
static inline uint8_t spi_byte(uint8_t outdat) __attribute__ ((always_inline));
static inline uint8_t spi_byte(uint8_t outdat)
{
//#ifdef SPI_DMA
//	while(pdca_channel_network->tcrr || pdca_channel_network->tcr) {};	// wait until DMA transfers are completed
//#endif
	// Issue read command
	//spi_put8(ETH_SPI, outdat);
	while (!(AVR32_SPI.sr & AVR32_SPI_SR_TDRE_MASK)) {};		// should not block .. theoretically ...
	AVR32_SPI.tdr = outdat << AVR32_SPI_TDR_TD_OFFSET;

	//xdata=spi_get8(ETH_SPI);
	while ((AVR32_SPI.sr & (AVR32_SPI_SR_RDRF_MASK | AVR32_SPI_SR_TXEMPTY_MASK)) != (AVR32_SPI_SR_RDRF_MASK | AVR32_SPI_SR_TXEMPTY_MASK)) {};	// wait for character to appear
	return (AVR32_SPI.rdr >> AVR32_SPI_RDR_RD_OFFSET);

	//return (xdata);
}

#define SPI_BEGIN		0
#define SPI_CONTINUE	1
#define SPI_END			2
#define SPI_COMPLETE	3

/* spi_op() performs register reads, register writes, FIFO reads, and
 * FIFO writes.  It can also either:
 * Do one complete SPI transfer (with CSN bracketing all of the SPI bytes),
 * Start an SPI transfer (asserting CSN but not negating it),
 * Continue an SPI transfer (leaving CSN in the asserted state), or
 * End an SPI transfer (negating CSN at the end of the transfer).
 */

/*
	To maintain reasonable speed, this code is optimized and inlined, so the assembly output of the whole thing would be tight!
*/

static void spi_op(uint8_t phase, uint16_t cmd, uint8_t *buf, uint16_t len)
{
uint16_t	opcode;
uint16_t	ii;

	opcode = cmd & OPCODE_MASK;

	if ((phase == SPI_BEGIN) || (phase == SPI_COMPLETE))
	{
		// clear all chipselects
		AVR32_SPI.mr |= AVR32_SPI_MR_PCS_MASK;
		// Drop CSN
		AVR32_SPI.mr &= ~((1 << (AVR32_SPI_MR_PCS_OFFSET + ETH_SPI_NPCS))|AVR32_SPI_MR_PCSDEC_MASK);		// mask chipselect + peripheral decode bit (we have no mux)
		//select_eth();

		// Command phase
		spi_byte(cmd >> 8);
		if ((opcode == IO_RD) || (opcode == IO_WR))
		{
			// Do extra byte for command phase
			spi_byte(cmd & 0xff);
		}
	}

   // Data phase
	if ((opcode == IO_RD) || (opcode == FIFO_RD))
	{
#ifdef DMA_RX
//
#error This implementation is not working (have not been able to figure out yet, why)!
//
		while(!(AVR32_SPI.sr & AVR32_SPI_SR_TDRE_MASK)) {};			// Wait until any non-DMA operation is complete. Should not block .. theoretically ...
		pdca_reload_channel(PDCA_CHANNEL_2, (void *)buf, len);		// set up receiver
		while(pdca_channel_network_rx->tcr) {};						// block until DMA transfer is complete
#else
		for (ii = 0; ii < len; ii++)
		{
			*buf++ = spi_byte(0);
		}
#endif
	}
	else
	{
#ifdef SPI_DMA
		while(!(AVR32_SPI.sr & AVR32_SPI_SR_TDRE_MASK)) {};			// Wait until any non-DMA operation is complete. Should not block .. theoretically ...
		pdca_reload_channel(PDCA_CHANNEL_1, (void *)buf, len);		// initiate DMA transfer
		while(pdca_channel_network_tx->tcr) {};						// block until DMA transfer is complete
#else
		for (ii = 0; ii < len; ii++)
		{
			spi_byte(*buf++);
		}
#endif
	}

	if ((phase == SPI_END) || (phase == SPI_COMPLETE))
	{
		//while (!(spi->sr & AVR32_SPI_SR_TXEMPTY_MASK)) {};		// can we stall here somehow??
		// Last transfer, so deassert the current NPCS if CSAAT is set.
		AVR32_SPI.cr = AVR32_SPI_CR_LASTXFER_MASK;			// ask SPI to de-assert all chipselects after last transfer iscomplete (saves us some sycles of polling for transfer complete)
		// Negate CSN
		//AVR32_SPI.mr |= AVR32_SPI_MR_PCS_MASK;
		//unselect_eth();
	}
}

/*
The same as previous, but does not wait for the DMA transfer to complete
*/
static void spi_op_nonblocking(uint8_t phase, uint16_t cmd, uint8_t *buf, uint16_t len)
{
uint16_t	opcode;
uint16_t	ii;

	opcode = cmd & OPCODE_MASK;

	if ((phase == SPI_BEGIN) || (phase == SPI_COMPLETE))
	{
		// clear all chipselects
		AVR32_SPI.mr |= AVR32_SPI_MR_PCS_MASK;
		// Drop CSN
		AVR32_SPI.mr &= ~((1 << (AVR32_SPI_MR_PCS_OFFSET + ETH_SPI_NPCS))|AVR32_SPI_MR_PCSDEC_MASK);		// mask chipselect + peripheral decode bit (we have no mux)
		//select_eth();

		// Command phase
		spi_byte(cmd >> 8);
		if ((opcode == IO_RD) || (opcode == IO_WR))
		{
			// Do extra byte for command phase
			spi_byte(cmd & 0xff);
		}
	}

   // Data phase
	if ((opcode == IO_RD) || (opcode == FIFO_RD))
	{
#ifdef DMA_RX
//
#error This implementation is not working (have not been able to figure out yet, why)!
//
		while(!(AVR32_SPI.sr & AVR32_SPI_SR_TDRE_MASK)) {};			// Wait until any non-DMA operation is complete. Should not block .. theoretically ...
		pdca_reload_channel(PDCA_CHANNEL_2, (void *)buf, len);		// set up receiver
		//while(pdca_channel_network_rx->tcr) {};						// block until DMA transfer is complete
#else
		for (ii = 0; ii < len; ii++)
		{
			*buf++ = spi_byte(0);
		}
#endif
	}
	else
	{
#ifdef SPI_DMA
		while(!(AVR32_SPI.sr & AVR32_SPI_SR_TDRE_MASK)) {};			// Wait until any non-DMA operation is complete. Should not block .. theoretically ...
		pdca_reload_channel(PDCA_CHANNEL_1, (void *)buf, len);		// initiate DMA transfer
		//while(pdca_channel_network_tx->tcr) {};						// block until DMA transfer is complete
#else
		for (ii = 0; ii < len; ii++)
		{
			spi_byte(*buf++);
		}
#endif
	}

	if ((phase == SPI_END) || (phase == SPI_COMPLETE))
	{
		//while (!(spi->sr & AVR32_SPI_SR_TXEMPTY_MASK)) {};		// can we stall here somehow??
		// Last transfer, so deassert the current NPCS if CSAAT is set.
		AVR32_SPI.cr = AVR32_SPI_CR_LASTXFER_MASK;			// ask SPI to de-assert all chipselects after last transfer iscomplete (saves us some sycles of polling for transfer complete)
		// Negate CSN
		//AVR32_SPI.mr |= AVR32_SPI_MR_PCS_MASK;
		//unselect_eth();
	}
}



/* ksz8851_regrd() will read one 16-bit word from reg. */
uint16_t ksz8851_regrd(uint16_t reg)
{
   uint16_t	cmd;
   uint8_t	inbuf[2];
   uint16_t	rddata;

   /* Move register address to cmd bits 9-2, make 32-bit address */
   cmd = (reg << 2) & 0x3f0;

   /* Add byte enables to cmd */
   if (reg & 2) {
	/* Odd word address writes bytes 2 and 3 */
	cmd |= (0xc << 10);
   } else {
	/* Even word address write bytes 0 and 1 */
	cmd |= (0x3 << 10);
   }

   /* Add opcode to cmd */
   cmd |= IO_RD;

   spi_op(SPI_COMPLETE, cmd, inbuf, 2);

   /* Byte 0 is first in, byte 1 is next */
   rddata = (inbuf[1] << 8) | inbuf[0];

   //printf("ksz8851_regrd: reading 0x%.4x from 0x%.4x\n", rddata, reg);

   return rddata;
}

/* ksz8851_regwr() will write one 16-bit word (wrdata) to reg. */
void
ksz8851_regwr(uint16_t reg, uint16_t wrdata)
{
   uint16_t	cmd;
   uint8_t	outbuf[2];

   //printf("ksz8851_regwr: writing 0x%.4x  to  0x%.4x\n", wrdata, reg);

   /* Move register address to cmd bits 9-2, make 32-bit address */
   cmd = (reg << 2) & 0x3f0;

   /* Add byte enables to cmd */
   if (reg & 2) {
	/* Odd word address writes bytes 2 and 3 */
	cmd |= (0xc << 10);
   } else {
	/* Even word address write bytes 0 and 1 */
	cmd |= (0x3 << 10);
   }

   /* Add opcode to cmd */
   cmd |= IO_WR;

   /* Byte 0 is first out, byte 1 is next */
   outbuf[0] = wrdata & 0xff;
   outbuf[1] = wrdata >> 8;

   spi_op(SPI_COMPLETE, cmd, outbuf, 2);
}

/* spi_setbits() will set all of the bits in bits_to_set in register
 * reg. */
void
spi_setbits(uint16_t reg, uint16_t bits_to_set)
{
   uint16_t	temp;

   temp = ksz8851_regrd(reg);
   temp |= bits_to_set;
   ksz8851_regwr(reg, temp);
}

/* spi_clrbits() will clear all of the bits in bits_to_clr in register
 * reg. */
void
spi_clrbits(uint16_t reg, uint16_t bits_to_clr)
{
   uint16_t	temp;

   temp = ksz8851_regrd(reg);
   temp &= ~bits_to_clr;
   ksz8851_regwr(reg, temp);
}

/* ksz8851Init() initializes the ksz8851.
 */
void
ksz8851Init(void)
{
   uint16_t	dev_id;
   int i;

   init_spi();

   /* Make sure we get a valid chip ID before going on */
   for (i=0; i<20; i++)
   {
	/* Perform Global Soft Reset */
	spi_setbits(REG_RESET_CTRL, GLOBAL_SOFTWARE_RESET);
	delayms(10);
	spi_clrbits(REG_RESET_CTRL, GLOBAL_SOFTWARE_RESET);

	delayms(200);

	/* Read device chip ID */
	dev_id = ksz8851_regrd(REG_CHIP_ID);

	if ((dev_id & 0xFFF0) == CHIP_ID_8851_16)
		break;
   }

   /* Write QMU MAC address (low) */
   ksz8851_regwr(REG_MAC_ADDR_01, (KSZ8851_MAC4 << 8) | KSZ8851_MAC5);
   /* Write QMU MAC address (middle) */
   ksz8851_regwr(REG_MAC_ADDR_23, (KSZ8851_MAC2 << 8) | KSZ8851_MAC3);
   /* Write QMU MAC address (high) */
   ksz8851_regwr(REG_MAC_ADDR_45, (KSZ8851_MAC0 << 8) | KSZ8851_MAC1);

   /* Enable QMU Transmit Frame Data Pointer Auto Increment */
   ksz8851_regwr(REG_TX_ADDR_PTR, ADDR_PTR_AUTO_INC);

   /* Enable QMU TxQ Auto-Enqueue frame */
   //ksz8851_regwr(REG_TXQ_CMD, TXQ_AUTO_ENQUEUE);

   /* Enable QMU Transmit:
    * flow control,
    * padding,
    * CRC, and
    * IP/TCP/UDP/ICMP checksum generation.
    */
   ksz8851_regwr(REG_TX_CTRL, DEFAULT_TX_CTRL);

   /* Enable QMU Receive Frame Data Pointer Auto Increment */
   ksz8851_regwr(REG_RX_ADDR_PTR, ADDR_PTR_AUTO_INC);

   /* Configure Receive Frame Threshold for one frame */
   ksz8851_regwr(REG_RX_FRAME_CNT_THRES, 1);

   /* Enable QMU Receive:
    * flow control,
    * receive all broadcast frames,
    * receive unicast frames, and
    * IP/TCP/UDP/ICMP checksum generation.
    */
   ksz8851_regwr(REG_RX_CTRL1, DEFAULT_RX_CTRL1);

   /* Enable QMU Receive:
    * ICMP/UDP Lite frame checksum verification,
    * UDP Lite frame checksum generation, and
    * IPv6 UDP fragment frame pass.
    */
   ksz8851_regwr(REG_RX_CTRL2, DEFAULT_RX_CTRL2 | RX_CTRL_BURST_LEN_FRAME);

   /* Enable QMU Receive:
    * IP Header Two-Byte Offset,
    * Receive Frame Count Threshold, and
    * RXQ Auto-Dequeue frame.
    */
   ksz8851_regwr(REG_RXQ_CMD, RXQ_CMD_CNTL);

   /* restart Port 1 auto-negotiation */
   spi_setbits(REG_PORT_CTRL, PORT_AUTO_NEG_RESTART);

   /* Clear the interrupts status */
   ksz8851_regwr(REG_INT_STATUS, 0xffff);

   /* Enable QMU Transmit */
   spi_setbits(REG_TX_CTRL, TX_CTRL_ENABLE);

   /* Enable QMU Receive */
   spi_setbits(REG_RX_CTRL1, RX_CTRL_ENABLE);

#if MULTI_FRAME
   /* Configure Receive Frame Threshold for 4 frames */
   ksz8851_regwr(REG_RX_FRAME_CNT_THRES, 4);

   /* Configure Receive Duration Threshold for 1 ms */
   ksz8851_regwr(REG_RX_TIME_THRES, 0x03e8);

   /* Enable
    * QMU Recieve IP Header Two-Byte Offset,
    * Receive Frame Count Threshold,
    * Receive Duration Timer Threshold, and
    * RXQ Auto-Dequeue frame.
    */
   ksz8851_regwr(REG_RXQ_CMD,
	    RXQ_TWOBYTE_OFFSET |
	    RXQ_TIME_INT | RXQ_FRAME_CNT_INT | RXQ_AUTO_DEQUEUE);
#endif
}

/* ksz8851BeginPacketSend() starts the packet sending process.  First,
 * it checks to see if there's enough space in the ksz8851 to send the
 * packet.  If not, it waits until there is enough room.
 * Once there is enough room, it enables TXQ write access and sends
 * the 4-byte control word to the ksz8851.
 */
void
ksz8851BeginPacketSend(unsigned int packetLength)
{
   uint16_t	txmir;
   uint16_t	isr;
   uint8_t	outbuf[4];

   /* Check if TXQ memory size is available for this transmit packet */
   txmir = ksz8851_regrd(REG_TX_MEM_INFO) & TX_MEM_AVAILABLE_MASK;
   if (txmir < packetLength + 4) {
	/* Not enough space to send packet. */

	/* Enable TXQ memory available monitor */
	ksz8851_regwr(REG_TX_TOTAL_FRAME_SIZE, packetLength + 4);

	spi_setbits(REG_TXQ_CMD, TXQ_MEM_AVAILABLE_INT);

	/* When the isr register has the TXSAIS bit set, there's
	* enough room for the packet.
	*/
	do {
	   isr = ksz8851_regrd(REG_INT_STATUS);
	} while (!(isr & INT_TX_SPACE));

	/* Disable TXQ memory available monitor */
	spi_clrbits(REG_TXQ_CMD, TXQ_MEM_AVAILABLE_INT);

	/* Clear the flag */
	isr &= ~INT_TX_SPACE;
	ksz8851_regwr(REG_INT_STATUS, isr);
   }

   //???? PORTD |= _BV(PD6);

   /* Enable TXQ write access */
   spi_setbits(REG_RXQ_CMD, RXQ_START);

   /* Write control word and byte count */
   outbuf[0] = frameID++ & 0x3f;
   outbuf[1] = 0;
   outbuf[2] = packetLength & 0xff;
   outbuf[3] = packetLength >> 8;

#if 0
   printf("ksz8851BeginPacketSend():\n0x%.2x 0x%.2x 0x%.2x 0x%.2x\n",
	  outbuf[0], outbuf[1], outbuf[2], outbuf[3]);
#endif

   spi_op(SPI_BEGIN, FIFO_WR, outbuf, 4);

   length_sum = 0;
}

/* ksz8851SendPacketData() is used to send the payload of the packet.
 * It may be called one or more times to completely transfer the
 * packet.
*/
void
ksz8851SendPacketData(unsigned char *localBuffer, unsigned int length)
{
#if 0
   int ii;
   unsigned char *p;

   printf("ksz8851SendPacketData():length = %d\n", length);
   p = localBuffer;
   for (ii = 0; ii < length; ii++) {
	printf("0x%.2x ", *p++);
	if ((ii % 8) == 7) {
	   printf("\n");
	}
   }
   printf("\n");
#endif

   length_sum += length;

   spi_op(SPI_CONTINUE, FIFO_WR, localBuffer, length);
}

/* ksz8851SendPacketDataNonBlocking() is used to send the payload of the packet.
 * It may be called one or more times to completely transfer the
 * packet.
 * Instead of spi_op(). it calls spi_op_nonblocking, what does not waituntil DMA transfer is complete, but
 * returns after the transfer is initiated.
 * Calling function has to check the transfer completion using spi_tx_completed() or spi_rx_completed() functions.
 * If DMA transfr is not enabled, the blocking and nonblocking versions behave the same.
*/
void
ksz8851SendPacketDataNonBlocking(unsigned char *localBuffer, unsigned int length)
{
   length_sum += length;
   spi_op_nonblocking(SPI_CONTINUE, FIFO_WR, localBuffer, length);
}


/* ksz8851EndPacketSend() is called to complete the sending of the
 * packet.  It pads the payload to round it up to the nearest DWORD,
 * then it diables the TXQ write access and isues the transmit command
 * to the TXQ.  Finally, it waits for the transmit command to complete
 * before exiting.
*/
void
ksz8851EndPacketSend(void)
{
   uint32_t	dummy = 0;

   //printf("ksz8851EndPacketSend():length_sum = %d\n", length_sum);

   /* Calculate how many bytes to get to DWORD */
   length_sum = -length_sum & 3;

   //printf("ksz8851EndPacketSend():extra bytes = %d\n", length_sum);

   /* Finish SPI FIFO_WR transaction */
   spi_op(SPI_END, FIFO_WR, (uint8_t *)&dummy, length_sum);

   /* Disable TXQ write access */
   spi_clrbits(REG_RXQ_CMD, RXQ_START);

   /* Issue transmit command to the TXQ */
   spi_setbits(REG_TXQ_CMD, TXQ_ENQUEUE);

   /* Wait until transmit command clears */
   while (ksz8851_regrd(REG_TXQ_CMD) & TXQ_ENQUEUE)
	;

   //????? PORTD &= ~_BV(PD6);
}

/* ksz8851Overrun() -- Needs work */
static void
ksz8851Overrun(void)
{
   //printf("ksz8851_overrun\n");
}

/* ksz8851ProcessInterrupt() -- All this does (for now) is check for
 * an overrun condition.
*/
static void
ksz8851ProcessInterrupt(void)
{
   uint16_t	isr;

   isr = ksz8851_regrd(REG_INT_STATUS);

   if( isr & INT_RX_OVERRUN ) {
	/* Clear the flag */
	isr &= ~INT_RX_OVERRUN;
	ksz8851_regwr(REG_INT_STATUS, isr);

	ksz8851Overrun();
   }
}

/* ksz8851BeginPacketRetrieve() checks to see if there are any packets
 * available.  If not, it returns 0.
 * If there are packets available, it gets the number of packets
 * available and the length of the first packet.  If there are any
 * errors in the packet, it releases that packet from the ksz8851.
 * It then sets up the ksz8851 for RXQ read access, reads the first
 * DWORD (which is garbage), then reads the 4-byte status word/byte
 * count, then the 2-byte alignment word.
 * Finally, it returns the length of the packet (without the CRC
 * trailer).
*/
unsigned int
ksz8851BeginPacketRetrieve(void)
{
   static uint8_t rxFrameCount = 0;
   uint16_t	rxfctr, rxfhsr;
   int16_t	rxPacketLength;
   uint8_t	dummy[4];

   if (rxFrameCount == 0) {
	ksz8851ProcessInterrupt();

	if (!(ksz8851_regrd(REG_INT_STATUS) & INT_RX)) {
	   /* No packets available */
	   return 0;
	}

	/* Clear Rx flag */
	spi_setbits(REG_INT_STATUS, INT_RX);

	/* Read rx total frame count */
	rxfctr = ksz8851_regrd(REG_RX_FRAME_CNT_THRES);
	rxFrameCount = (rxfctr & RX_FRAME_CNT_MASK) >> 8;

	if (rxFrameCount == 0) {
	   return 0;
	}
   }

   //????? PORTD |= _BV(PD5);

   /* read rx frame header status */
   rxfhsr = ksz8851_regrd(REG_RX_FHR_STATUS);

   //printf("rxfhsr = 0x%x\n", rxfhsr);

   if (rxfhsr & RX_ERRORS) {
	/* Packet has errors */
	//printf("rx errors: rxfhsr = 0x%x\n", rxfhsr);

	/* Issue the RELEASE error frame command */
	spi_setbits(REG_RXQ_CMD, RXQ_CMD_FREE_PACKET);

	rxFrameCount--;

	return 0;
   }

   /* Read byte count (4-byte CRC included) */
   rxPacketLength = ksz8851_regrd(REG_RX_FHR_BYTE_CNT) & RX_BYTE_CNT_MASK;

   if (rxPacketLength <= 0) {
	//printf("Error: rxPacketLength = %d\n", rxPacketLength);

	/* Issue the RELEASE error frame command */
	spi_setbits(REG_RXQ_CMD, RXQ_CMD_FREE_PACKET);

	rxFrameCount--;

	return 0;
   }

   /* Clear rx frame pointer */
   spi_clrbits(REG_RX_ADDR_PTR, ADDR_PTR_MASK);

   /* Enable RXQ read access */
   spi_setbits(REG_RXQ_CMD, RXQ_START);

#if 0
   printf("rx packet received, frm_cnt = %d, byte_cnt = 0x%.2x\n",
	  rxFrameCount, rxPacketLength);
#endif

   /* Read 4-byte garbage */
   spi_op(SPI_BEGIN, FIFO_RD, dummy, 4);

   /* Read 4-byte status word/byte count */
   spi_op(SPI_CONTINUE, FIFO_RD, dummy, 4);

#if 0
   printf("stat[0] = 0x%.2x, stat[1] = 0x%.2x, bytecnt[0] = 0x%.2x, bytecnt[1] = 0x%.2x\n",
	  dummy[0], dummy[1], dummy[2], dummy[3]);
#endif

   /* Read 2-byte alignment bytes */
   spi_op(SPI_CONTINUE, FIFO_RD, dummy, 2);

   rxFrameCount--;

   return rxPacketLength - 4;
}

/* ksz8851RetrievePacketData() is used to retrieve the payload of a
 * packet.  It may be called as many times as necessary to retrieve
 * the entire payload.
 */
void
ksz8851RetrievePacketData(unsigned char * localBuffer, unsigned int length)
{
   spi_op(SPI_CONTINUE, FIFO_RD, localBuffer, length);

#if 0
   {
	int ii;
	unsigned char *p;
	printf("ksz8851RetreivePacketData():\n");

	p = localBuffer;
	for (ii = 0; ii < length; ii++) {
	   printf("0x%.2x ", *p++);
	   if ((ii % 8) == 7) {
		printf("\n");
	   }
	}
	printf("\n");
   }
#endif
}

/* ksz8851EndPacketRetrieve() reads (and discards) the 4-byte CRC,
 * and ends the RXQ read access.
 */
void
ksz8851EndPacketRetrieve(void)
{
   uint8_t	crc[4];

   /* Read 4-byte crc */
   spi_op(SPI_END, FIFO_RD, crc, 4);

#if 0
   printf("crc[0] = 0x%.2x, crc[1] = 0x%.2x, crc[2] = 0x%.2x, crc[3] = 0x%.2x\n",
	  crc[0], crc[1], crc[2], crc[3]);
#endif

   /* End RXQ read access */
   spi_clrbits(REG_RXQ_CMD, RXQ_START);

   // ???? PORTD &= ~_BV(PD5);
}
