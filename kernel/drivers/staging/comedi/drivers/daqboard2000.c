

#include "../comedidev.h"

#include <linux/delay.h>
#include <linux/interrupt.h>

#include "comedi_pci.h"
#include "8255.h"

#define DAQBOARD2000_SUBSYSTEM_IDS2 	0x00021616	/* Daqboard/2000 - 2 Dacs */
#define DAQBOARD2000_SUBSYSTEM_IDS4 	0x00041616	/* Daqboard/2000 - 4 Dacs */

#define DAQBOARD2000_DAQ_SIZE 		0x1002
#define DAQBOARD2000_PLX_SIZE 		0x100

/* Initialization bits for the Serial EEPROM Control Register */
#define DAQBOARD2000_SECRProgPinHi      0x8001767e
#define DAQBOARD2000_SECRProgPinLo      0x8000767e
#define DAQBOARD2000_SECRLocalBusHi     0xc000767e
#define DAQBOARD2000_SECRLocalBusLo     0x8000767e
#define DAQBOARD2000_SECRReloadHi       0xa000767e
#define DAQBOARD2000_SECRReloadLo       0x8000767e

/* SECR status bits */
#define DAQBOARD2000_EEPROM_PRESENT     0x10000000

/* CPLD status bits */
#define DAQBOARD2000_CPLD_INIT 		0x0002
#define DAQBOARD2000_CPLD_DONE 		0x0004

/* Available ranges */
static const struct comedi_lrange range_daqboard2000_ai = { 13, {
								 RANGE(-10, 10),
								 RANGE(-5, 5),
								 RANGE(-2.5,
								       2.5),
								 RANGE(-1.25,
								       1.25),
								 RANGE(-0.625,
								       0.625),
								 RANGE(-0.3125,
								       0.3125),
								 RANGE(-0.156,
								       0.156),
								 RANGE(0, 10),
								 RANGE(0, 5),
								 RANGE(0, 2.5),
								 RANGE(0, 1.25),
								 RANGE(0,
								       0.625),
								 RANGE(0,
								       0.3125)
								 }
};

static const struct comedi_lrange range_daqboard2000_ao = { 1, {
								RANGE(-10, 10)
								}
};

struct daqboard2000_hw {
	volatile u16 acqControl;	/*  0x00 */
	volatile u16 acqScanListFIFO;	/*  0x02 */
	volatile u32 acqPacerClockDivLow;	/*  0x04 */

	volatile u16 acqScanCounter;	/*  0x08 */
	volatile u16 acqPacerClockDivHigh;	/*  0x0a */
	volatile u16 acqTriggerCount;	/*  0x0c */
	volatile u16 fill2;	/*  0x0e */
	volatile u16 acqResultsFIFO;	/*  0x10 */
	volatile u16 fill3;	/*  0x12 */
	volatile u16 acqResultsShadow;	/*  0x14 */
	volatile u16 fill4;	/*  0x16 */
	volatile u16 acqAdcResult;	/*  0x18 */
	volatile u16 fill5;	/*  0x1a */
	volatile u16 dacScanCounter;	/*  0x1c */
	volatile u16 fill6;	/*  0x1e */

	volatile u16 dacControl;	/*  0x20 */
	volatile u16 fill7;	/*  0x22 */
	volatile s16 dacFIFO;	/*  0x24 */
	volatile u16 fill8[2];	/*  0x26 */
	volatile u16 dacPacerClockDiv;	/*  0x2a */
	volatile u16 refDacs;	/*  0x2c */
	volatile u16 fill9;	/*  0x2e */

	volatile u16 dioControl;	/*  0x30 */
	volatile s16 dioP3hsioData;	/*  0x32 */
	volatile u16 dioP3Control;	/*  0x34 */
	volatile u16 calEepromControl;	/*  0x36 */
	volatile s16 dacSetting[4];	/*  0x38 */
	volatile s16 dioP2ExpansionIO8Bit[32];	/*  0x40 */

	volatile u16 ctrTmrControl;	/*  0x80 */
	volatile u16 fill10[3];	/*  0x82 */
	volatile s16 ctrInput[4];	/*  0x88 */
	volatile u16 fill11[8];	/*  0x90 */
	volatile u16 timerDivisor[2];	/*  0xa0 */
	volatile u16 fill12[6];	/*  0xa4 */

	volatile u16 dmaControl;	/*  0xb0 */
	volatile u16 trigControl;	/*  0xb2 */
	volatile u16 fill13[2];	/*  0xb4 */
	volatile u16 calEeprom;	/*  0xb8 */
	volatile u16 acqDigitalMark;	/*  0xba */
	volatile u16 trigDacs;	/*  0xbc */
	volatile u16 fill14;	/*  0xbe */
	volatile s16 dioP2ExpansionIO16Bit[32];	/*  0xc0 */
};

/* Scan Sequencer programming */
#define DAQBOARD2000_SeqStartScanList            0x0011
#define DAQBOARD2000_SeqStopScanList             0x0010

/* Prepare for acquisition */
#define DAQBOARD2000_AcqResetScanListFifo        0x0004
#define DAQBOARD2000_AcqResetResultsFifo         0x0002
#define DAQBOARD2000_AcqResetConfigPipe          0x0001

/* Acqusition status bits */
#define DAQBOARD2000_AcqResultsFIFOMore1Sample   0x0001
#define DAQBOARD2000_AcqResultsFIFOHasValidData  0x0002
#define DAQBOARD2000_AcqResultsFIFOOverrun       0x0004
#define DAQBOARD2000_AcqLogicScanning            0x0008
#define DAQBOARD2000_AcqConfigPipeFull           0x0010
#define DAQBOARD2000_AcqScanListFIFOEmpty        0x0020
#define DAQBOARD2000_AcqAdcNotReady              0x0040
#define DAQBOARD2000_ArbitrationFailure          0x0080
#define DAQBOARD2000_AcqPacerOverrun             0x0100
#define DAQBOARD2000_DacPacerOverrun             0x0200
#define DAQBOARD2000_AcqHardwareError            0x01c0

/* Scan Sequencer programming */
#define DAQBOARD2000_SeqStartScanList            0x0011
#define DAQBOARD2000_SeqStopScanList             0x0010

/* Pacer Clock Control */
#define DAQBOARD2000_AdcPacerInternal            0x0030
#define DAQBOARD2000_AdcPacerExternal            0x0032
#define DAQBOARD2000_AdcPacerEnable              0x0031
#define DAQBOARD2000_AdcPacerEnableDacPacer      0x0034
#define DAQBOARD2000_AdcPacerDisable             0x0030
#define DAQBOARD2000_AdcPacerNormalMode          0x0060
#define DAQBOARD2000_AdcPacerCompatibilityMode   0x0061
#define DAQBOARD2000_AdcPacerInternalOutEnable   0x0008
#define DAQBOARD2000_AdcPacerExternalRising      0x0100

/* DAC status */
#define DAQBOARD2000_DacFull                     0x0001
#define DAQBOARD2000_RefBusy                     0x0002
#define DAQBOARD2000_TrgBusy                     0x0004
#define DAQBOARD2000_CalBusy                     0x0008
#define DAQBOARD2000_Dac0Busy                    0x0010
#define DAQBOARD2000_Dac1Busy                    0x0020
#define DAQBOARD2000_Dac2Busy                    0x0040
#define DAQBOARD2000_Dac3Busy                    0x0080

/* DAC control */
#define DAQBOARD2000_Dac0Enable                  0x0021
#define DAQBOARD2000_Dac1Enable                  0x0031
#define DAQBOARD2000_Dac2Enable                  0x0041
#define DAQBOARD2000_Dac3Enable                  0x0051
#define DAQBOARD2000_DacEnableBit                0x0001
#define DAQBOARD2000_Dac0Disable                 0x0020
#define DAQBOARD2000_Dac1Disable                 0x0030
#define DAQBOARD2000_Dac2Disable                 0x0040
#define DAQBOARD2000_Dac3Disable                 0x0050
#define DAQBOARD2000_DacResetFifo                0x0004
#define DAQBOARD2000_DacPatternDisable           0x0060
#define DAQBOARD2000_DacPatternEnable            0x0061
#define DAQBOARD2000_DacSelectSignedData         0x0002
#define DAQBOARD2000_DacSelectUnsignedData       0x0000

/* Trigger Control */
#define DAQBOARD2000_TrigAnalog                  0x0000
#define DAQBOARD2000_TrigTTL                     0x0010
#define DAQBOARD2000_TrigTransHiLo               0x0004
#define DAQBOARD2000_TrigTransLoHi               0x0000
#define DAQBOARD2000_TrigAbove                   0x0000
#define DAQBOARD2000_TrigBelow                   0x0004
#define DAQBOARD2000_TrigLevelSense              0x0002
#define DAQBOARD2000_TrigEdgeSense               0x0000
#define DAQBOARD2000_TrigEnable                  0x0001
#define DAQBOARD2000_TrigDisable                 0x0000

/* Reference Dac Selection */
#define DAQBOARD2000_PosRefDacSelect             0x0100
#define DAQBOARD2000_NegRefDacSelect             0x0000

static int daqboard2000_attach(struct comedi_device *dev,
			       struct comedi_devconfig *it);
static int daqboard2000_detach(struct comedi_device *dev);

static struct comedi_driver driver_daqboard2000 = {
	.driver_name = "daqboard2000",
	.module = THIS_MODULE,
	.attach = daqboard2000_attach,
	.detach = daqboard2000_detach,
};

struct daq200_boardtype {
	const char *name;
	int id;
};
static const struct daq200_boardtype boardtypes[] = {
	{"ids2", DAQBOARD2000_SUBSYSTEM_IDS2},
	{"ids4", DAQBOARD2000_SUBSYSTEM_IDS4},
};

#define n_boardtypes (sizeof(boardtypes)/sizeof(struct daq200_boardtype))
#define this_board ((const struct daq200_boardtype *)dev->board_ptr)

static DEFINE_PCI_DEVICE_TABLE(daqboard2000_pci_table) = {
	{
	0x1616, 0x0409, PCI_ANY_ID, PCI_ANY_ID, 0, 0, 0}, {
	0}
};

MODULE_DEVICE_TABLE(pci, daqboard2000_pci_table);

struct daqboard2000_private {
	enum {
		card_daqboard_2000
	} card;
	struct pci_dev *pci_dev;
	void *daq;
	void *plx;
	int got_regions;
	unsigned int ao_readback[2];
};

#define devpriv ((struct daqboard2000_private *)dev->private)

static void writeAcqScanListEntry(struct comedi_device *dev, u16 entry)
{
	struct daqboard2000_hw *fpga = devpriv->daq;

/* udelay(4); */
	fpga->acqScanListFIFO = entry & 0x00ff;
/* udelay(4); */
	fpga->acqScanListFIFO = (entry >> 8) & 0x00ff;
}

static void setup_sampling(struct comedi_device *dev, int chan, int gain)
{
	u16 word0, word1, word2, word3;

	/* Channel 0-7 diff, channel 8-23 single ended */
	word0 = 0;
	word1 = 0x0004;		/* Last scan */
	word2 = (chan << 6) & 0x00c0;
	switch (chan / 4) {
	case 0:
		word3 = 0x0001;
		break;
	case 1:
		word3 = 0x0002;
		break;
	case 2:
		word3 = 0x0005;
		break;
	case 3:
		word3 = 0x0006;
		break;
	case 4:
		word3 = 0x0041;
		break;
	case 5:
		word3 = 0x0042;
		break;
	default:
		word3 = 0;
		break;
	}
	/* These should be read from EEPROM */
	word2 |= 0x0800;
	word3 |= 0xc000;
/*  printk("%d %4.4x %4.4x %4.4x %4.4x\n", chan, word0, word1, word2, word3);*/
	writeAcqScanListEntry(dev, word0);
	writeAcqScanListEntry(dev, word1);
	writeAcqScanListEntry(dev, word2);
	writeAcqScanListEntry(dev, word3);
}

static int daqboard2000_ai_insn_read(struct comedi_device *dev,
				     struct comedi_subdevice *s,
				     struct comedi_insn *insn,
				     unsigned int *data)
{
	int i;
	struct daqboard2000_hw *fpga = devpriv->daq;
	int gain, chan, timeout;

	fpga->acqControl =
	    DAQBOARD2000_AcqResetScanListFifo |
	    DAQBOARD2000_AcqResetResultsFifo | DAQBOARD2000_AcqResetConfigPipe;

	/* If pacer clock is not set to some high value (> 10 us), we
	   risk multiple samples to be put into the result FIFO. */
	fpga->acqPacerClockDivLow = 1000000;	/* 1 second, should be long enough */
	fpga->acqPacerClockDivHigh = 0;

	gain = CR_RANGE(insn->chanspec);
	chan = CR_CHAN(insn->chanspec);

	/* This doesn't look efficient.  I decided to take the conservative
	 * approach when I did the insn conversion.  Perhaps it would be
	 * better to have broken it completely, then someone would have been
	 * forced to fix it.  --ds */
	for (i = 0; i < insn->n; i++) {
		setup_sampling(dev, chan, gain);
		/* Enable reading from the scanlist FIFO */
		fpga->acqControl = DAQBOARD2000_SeqStartScanList;
		for (timeout = 0; timeout < 20; timeout++) {
			if (fpga->acqControl & DAQBOARD2000_AcqConfigPipeFull) {
				break;
			}
			/* udelay(2); */
		}
		fpga->acqControl = DAQBOARD2000_AdcPacerEnable;
		for (timeout = 0; timeout < 20; timeout++) {
			if (fpga->acqControl & DAQBOARD2000_AcqLogicScanning) {
				break;
			}
			/* udelay(2); */
		}
		for (timeout = 0; timeout < 20; timeout++) {
			if (fpga->acqControl &
			    DAQBOARD2000_AcqResultsFIFOHasValidData) {
				break;
			}
			/* udelay(2); */
		}
		data[i] = fpga->acqResultsFIFO;
		fpga->acqControl = DAQBOARD2000_AdcPacerDisable;
		fpga->acqControl = DAQBOARD2000_SeqStopScanList;
	}

	return i;
}

static int daqboard2000_ao_insn_read(struct comedi_device *dev,
				     struct comedi_subdevice *s,
				     struct comedi_insn *insn,
				     unsigned int *data)
{
	int i;
	int chan = CR_CHAN(insn->chanspec);

	for (i = 0; i < insn->n; i++) {
		data[i] = devpriv->ao_readback[chan];
	}

	return i;
}

static int daqboard2000_ao_insn_write(struct comedi_device *dev,
				      struct comedi_subdevice *s,
				      struct comedi_insn *insn,
				      unsigned int *data)
{
	int i;
	int chan = CR_CHAN(insn->chanspec);
	struct daqboard2000_hw *fpga = devpriv->daq;
	int timeout;

	for (i = 0; i < insn->n; i++) {
		/*
		 * OK, since it works OK without enabling the DAC's, let's keep
		 * it as simple as possible...
		 */
		/* fpga->dacControl = (chan + 2) * 0x0010 | 0x0001; udelay(1000); */
		fpga->dacSetting[chan] = data[i];
		for (timeout = 0; timeout < 20; timeout++) {
			if ((fpga->dacControl & ((chan + 1) * 0x0010)) == 0) {
				break;
			}
			/* udelay(2); */
		}
		devpriv->ao_readback[chan] = data[i];
		/*
		 * Since we never enabled the DAC's, we don't need to disable it...
		 * fpga->dacControl = (chan + 2) * 0x0010 | 0x0000; udelay(1000);
		 */
	}

	return i;
}

static void daqboard2000_resetLocalBus(struct comedi_device *dev)
{
	printk("daqboard2000_resetLocalBus\n");
	writel(DAQBOARD2000_SECRLocalBusHi, devpriv->plx + 0x6c);
	udelay(10000);
	writel(DAQBOARD2000_SECRLocalBusLo, devpriv->plx + 0x6c);
	udelay(10000);
}

static void daqboard2000_reloadPLX(struct comedi_device *dev)
{
	printk("daqboard2000_reloadPLX\n");
	writel(DAQBOARD2000_SECRReloadLo, devpriv->plx + 0x6c);
	udelay(10000);
	writel(DAQBOARD2000_SECRReloadHi, devpriv->plx + 0x6c);
	udelay(10000);
	writel(DAQBOARD2000_SECRReloadLo, devpriv->plx + 0x6c);
	udelay(10000);
}

static void daqboard2000_pulseProgPin(struct comedi_device *dev)
{
	printk("daqboard2000_pulseProgPin 1\n");
	writel(DAQBOARD2000_SECRProgPinHi, devpriv->plx + 0x6c);
	udelay(10000);
	writel(DAQBOARD2000_SECRProgPinLo, devpriv->plx + 0x6c);
	udelay(10000);		/* Not in the original code, but I like symmetry... */
}

static int daqboard2000_pollCPLD(struct comedi_device *dev, int mask)
{
	int result = 0;
	int i;
	int cpld;

	/* timeout after 50 tries -> 5ms */
	for (i = 0; i < 50; i++) {
		cpld = readw(devpriv->daq + 0x1000);
		if ((cpld & mask) == mask) {
			result = 1;
			break;
		}
		udelay(100);
	}
	udelay(5);
	return result;
}

static int daqboard2000_writeCPLD(struct comedi_device *dev, int data)
{
	int result = 0;

	udelay(10);
	writew(data, devpriv->daq + 0x1000);
	if ((readw(devpriv->daq + 0x1000) & DAQBOARD2000_CPLD_INIT) ==
	    DAQBOARD2000_CPLD_INIT) {
		result = 1;
	}
	return result;
}

static int initialize_daqboard2000(struct comedi_device *dev,
				   unsigned char *cpld_array, int len)
{
	int result = -EIO;
	/* Read the serial EEPROM control register */
	int secr;
	int retry;
	int i;

	/* Check to make sure the serial eeprom is present on the board */
	secr = readl(devpriv->plx + 0x6c);
	if (!(secr & DAQBOARD2000_EEPROM_PRESENT)) {
#ifdef DEBUG_EEPROM
		printk("no serial eeprom\n");
#endif
		return -EIO;
	}

	for (retry = 0; retry < 3; retry++) {
#ifdef DEBUG_EEPROM
		printk("Programming EEPROM try %x\n", retry);
#endif

		daqboard2000_resetLocalBus(dev);
		daqboard2000_reloadPLX(dev);
		daqboard2000_pulseProgPin(dev);
		if (daqboard2000_pollCPLD(dev, DAQBOARD2000_CPLD_INIT)) {
			for (i = 0; i < len; i++) {
				if (cpld_array[i] == 0xff
				    && cpld_array[i + 1] == 0x20) {
#ifdef DEBUG_EEPROM
					printk("Preamble found at %d\n", i);
#endif
					break;
				}
			}
			for (; i < len; i += 2) {
				int data =
				    (cpld_array[i] << 8) + cpld_array[i + 1];
				if (!daqboard2000_writeCPLD(dev, data)) {
					break;
				}
			}
			if (i >= len) {
#ifdef DEBUG_EEPROM
				printk("Programmed\n");
#endif
				daqboard2000_resetLocalBus(dev);
				daqboard2000_reloadPLX(dev);
				result = 0;
				break;
			}
		}
	}
	return result;
}

static void daqboard2000_adcStopDmaTransfer(struct comedi_device *dev)
{
/*  printk("Implement: daqboard2000_adcStopDmaTransfer\n");*/
}

static void daqboard2000_adcDisarm(struct comedi_device *dev)
{
	struct daqboard2000_hw *fpga = devpriv->daq;

	/* Disable hardware triggers */
	udelay(2);
	fpga->trigControl = DAQBOARD2000_TrigAnalog | DAQBOARD2000_TrigDisable;
	udelay(2);
	fpga->trigControl = DAQBOARD2000_TrigTTL | DAQBOARD2000_TrigDisable;

	/* Stop the scan list FIFO from loading the configuration pipe */
	udelay(2);
	fpga->acqControl = DAQBOARD2000_SeqStopScanList;

	/* Stop the pacer clock */
	udelay(2);
	fpga->acqControl = DAQBOARD2000_AdcPacerDisable;

	/* Stop the input dma (abort channel 1) */
	daqboard2000_adcStopDmaTransfer(dev);
}

static void daqboard2000_activateReferenceDacs(struct comedi_device *dev)
{
	struct daqboard2000_hw *fpga = devpriv->daq;
	int timeout;

	/*  Set the + reference dac value in the FPGA */
	fpga->refDacs = 0x80 | DAQBOARD2000_PosRefDacSelect;
	for (timeout = 0; timeout < 20; timeout++) {
		if ((fpga->dacControl & DAQBOARD2000_RefBusy) == 0) {
			break;
		}
		udelay(2);
	}
/*  printk("DAQBOARD2000_PosRefDacSelect %d\n", timeout);*/

	/*  Set the - reference dac value in the FPGA */
	fpga->refDacs = 0x80 | DAQBOARD2000_NegRefDacSelect;
	for (timeout = 0; timeout < 20; timeout++) {
		if ((fpga->dacControl & DAQBOARD2000_RefBusy) == 0) {
			break;
		}
		udelay(2);
	}
/*  printk("DAQBOARD2000_NegRefDacSelect %d\n", timeout);*/
}

static void daqboard2000_initializeCtrs(struct comedi_device *dev)
{
/*  printk("Implement: daqboard2000_initializeCtrs\n");*/
}

static void daqboard2000_initializeTmrs(struct comedi_device *dev)
{
/*  printk("Implement: daqboard2000_initializeTmrs\n");*/
}

static void daqboard2000_dacDisarm(struct comedi_device *dev)
{
/*  printk("Implement: daqboard2000_dacDisarm\n");*/
}

static void daqboard2000_initializeAdc(struct comedi_device *dev)
{
	daqboard2000_adcDisarm(dev);
	daqboard2000_activateReferenceDacs(dev);
	daqboard2000_initializeCtrs(dev);
	daqboard2000_initializeTmrs(dev);
}

static void daqboard2000_initializeDac(struct comedi_device *dev)
{
	daqboard2000_dacDisarm(dev);
}


static int daqboard2000_8255_cb(int dir, int port, int data,
				unsigned long ioaddr)
{
	int result = 0;
	if (dir) {
		writew(data, ((void *)ioaddr) + port * 2);
		result = 0;
	} else {
		result = readw(((void *)ioaddr) + port * 2);
	}
	return result;
}

static int daqboard2000_attach(struct comedi_device *dev,
			       struct comedi_devconfig *it)
{
	int result = 0;
	struct comedi_subdevice *s;
	struct pci_dev *card = NULL;
	void *aux_data;
	unsigned int aux_len;
	int bus, slot;

	printk("comedi%d: daqboard2000:", dev->minor);

	bus = it->options[0];
	slot = it->options[1];

	result = alloc_private(dev, sizeof(struct daqboard2000_private));
	if (result < 0) {
		return -ENOMEM;
	}
	for (card = pci_get_device(0x1616, 0x0409, NULL);
	     card != NULL; card = pci_get_device(0x1616, 0x0409, card)) {
		if (bus || slot) {
			/* requested particular bus/slot */
			if (card->bus->number != bus ||
			    PCI_SLOT(card->devfn) != slot) {
				continue;
			}
		}
		break;		/* found one */
	}
	if (!card) {
		if (bus || slot)
			printk(" no daqboard2000 found at bus/slot: %d/%d\n",
			       bus, slot);
		else
			printk(" no daqboard2000 found\n");
		return -EIO;
	} else {
		u32 id;
		int i;
		devpriv->pci_dev = card;
		id = ((u32) card->
		      subsystem_device << 16) | card->subsystem_vendor;
		for (i = 0; i < n_boardtypes; i++) {
			if (boardtypes[i].id == id) {
				printk(" %s", boardtypes[i].name);
				dev->board_ptr = boardtypes + i;
			}
		}
		if (!dev->board_ptr) {
			printk
			    (" unknown subsystem id %08x (pretend it is an ids2)",
			     id);
			dev->board_ptr = boardtypes;
		}
	}

	result = comedi_pci_enable(card, "daqboard2000");
	if (result < 0) {
		printk(" failed to enable PCI device and request regions\n");
		return -EIO;
	}
	devpriv->got_regions = 1;
	devpriv->plx =
	    ioremap(pci_resource_start(card, 0), DAQBOARD2000_PLX_SIZE);
	devpriv->daq =
	    ioremap(pci_resource_start(card, 2), DAQBOARD2000_DAQ_SIZE);
	if (!devpriv->plx || !devpriv->daq) {
		return -ENOMEM;
	}

	result = alloc_subdevices(dev, 3);
	if (result < 0)
		goto out;

	readl(devpriv->plx + 0x6c);

	/*
	   u8 interrupt;
	   Windows code does restore interrupts, but since we don't use them...
	   pci_read_config_byte(card, PCI_INTERRUPT_LINE, &interrupt);
	   printk("Interrupt before is: %x\n", interrupt);
	 */

	aux_data = comedi_aux_data(it->options, 0);
	aux_len = it->options[COMEDI_DEVCONF_AUX_DATA_LENGTH];

	if (aux_data && aux_len) {
		result = initialize_daqboard2000(dev, aux_data, aux_len);
	} else {
		printk("no FPGA initialization code, aborting\n");
		result = -EIO;
	}
	if (result < 0)
		goto out;
	daqboard2000_initializeAdc(dev);
	daqboard2000_initializeDac(dev);
	/*
	   Windows code does restore interrupts, but since we don't use them...
	   pci_read_config_byte(card, PCI_INTERRUPT_LINE, &interrupt);
	   printk("Interrupt after is: %x\n", interrupt);
	 */

	dev->iobase = (unsigned long)devpriv->daq;

	dev->board_name = this_board->name;

	s = dev->subdevices + 0;
	/* ai subdevice */
	s->type = COMEDI_SUBD_AI;
	s->subdev_flags = SDF_READABLE | SDF_GROUND;
	s->n_chan = 24;
	s->maxdata = 0xffff;
	s->insn_read = daqboard2000_ai_insn_read;
	s->range_table = &range_daqboard2000_ai;

	s = dev->subdevices + 1;
	/* ao subdevice */
	s->type = COMEDI_SUBD_AO;
	s->subdev_flags = SDF_WRITABLE;
	s->n_chan = 2;
	s->maxdata = 0xffff;
	s->insn_read = daqboard2000_ao_insn_read;
	s->insn_write = daqboard2000_ao_insn_write;
	s->range_table = &range_daqboard2000_ao;

	s = dev->subdevices + 2;
	result = subdev_8255_init(dev, s, daqboard2000_8255_cb,
				  (unsigned long)(dev->iobase + 0x40));

	printk("\n");
out:
	return result;
}

static int daqboard2000_detach(struct comedi_device *dev)
{
	printk("comedi%d: daqboard2000: remove\n", dev->minor);

	if (dev->subdevices)
		subdev_8255_cleanup(dev, dev->subdevices + 2);

	if (dev->irq) {
		free_irq(dev->irq, dev);
	}
	if (devpriv) {
		if (devpriv->daq)
			iounmap(devpriv->daq);
		if (devpriv->plx)
			iounmap(devpriv->plx);
		if (devpriv->pci_dev) {
			if (devpriv->got_regions) {
				comedi_pci_disable(devpriv->pci_dev);
			}
			pci_dev_put(devpriv->pci_dev);
		}
	}
	return 0;
}

COMEDI_PCI_INITCLEANUP(driver_daqboard2000, daqboard2000_pci_table);
