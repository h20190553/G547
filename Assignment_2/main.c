#include<linux/kernel.h>
#include<linux/module.h>
#include<linux/usb.h>
#include<linux/timer.h>	
#include<linux/slab.h>

#define SAN_VID  0x058f
#define SAN_PID  0x6387

#define HP_VID  0x03f0
#define HP_PID  0x9140

#define USB_EP_IN	0x80
#define USB_EP_OUT	0x00

#define READ_CAPACITY_LENGTH          0x08
#define RETRY_MAX                     5

#define BOMS_RESET		     0xFF
#define BOMS_RESET_REQ_TYPE          0x21

#define be_to_int32(buf) (((buf)[0]<<24)|((buf)[1]<<16)|((buf)[2]<<8)|(buf)[3])

int send_mass_storage_command(struct usb_device*, uint8_t, uint8_t,uint8_t*, uint8_t, int, uint32_t*);
int get_mass_storage_status(struct usb_device*, uint8_t, uint32_t);
int read_capacity(struct usb_device*, uint8_t, uint8_t);

struct command_block_wrapper {
	uint8_t dCBWSignature[4];
	uint32_t dCBWTag;
	uint32_t dCBWDataTransferLength;
	uint8_t bmCBWFlags;
	uint8_t bCBWLUN;
	uint8_t bCBWCBLength;
	uint8_t CBWCB[16];
};


struct command_status_wrapper {
	uint8_t dCSWSignature[4];
	uint32_t dCSWTag;
	uint32_t dCSWDataResidue;
	uint8_t bCSWStatus;
};


static uint8_t cdb_length[256] = {
//	 0  1  2  3  4  5  6  7  8  9  A  B  C  D  E  F
	06,06,06,06,06,06,06,06,06,06,06,06,06,06,06,06,  //  0
	06,06,06,06,06,06,06,06,06,06,06,06,06,06,06,06,  //  1
	10,10,10,10,10,10,10,10,10,10,10,10,10,10,10,10,  //  2
	10,10,10,10,10,10,10,10,10,10,10,10,10,10,10,10,  //  3
	10,10,10,10,10,10,10,10,10,10,10,10,10,10,10,10,  //  4
	10,10,10,10,10,10,10,10,10,10,10,10,10,10,10,10,  //  5
	00,00,00,00,00,00,00,00,00,00,00,00,00,00,00,00,  //  6
	00,00,00,00,00,00,00,00,00,00,00,00,00,00,00,00,  //  7
	16,16,16,16,16,16,16,16,16,16,16,16,16,16,16,16,  //  8
	16,16,16,16,16,16,16,16,16,16,16,16,16,16,16,16,  //  9
	12,12,12,12,12,12,12,12,12,12,12,12,12,12,12,12,  //  A
	12,12,12,12,12,12,12,12,12,12,12,12,12,12,12,12,  //  B
	00,00,00,00,00,00,00,00,00,00,00,00,00,00,00,00,  //  C
	00,00,00,00,00,00,00,00,00,00,00,00,00,00,00,00,  //  D
	00,00,00,00,00,00,00,00,00,00,00,00,00,00,00,00,  //  E
	00,00,00,00,00,00,00,00,00,00,00,00,00,00,00,00,  //  F
};

struct usbdev_private
{
	struct usb_device *udev;
	unsigned char class;
	unsigned char subclass;
	unsigned char protocol;
	unsigned char ep_in;
	unsigned char ep_out;
};

struct usbdev_private *p_usbdev_info;

static void usbdev_disconnect(struct usb_interface *interface)
{
	printk(KERN_INFO "USBDEV Device Removed\n");
	return;
}

static struct usb_device_id usbdev_table [] = {
	{USB_DEVICE(SAN_VID, SAN_PID)},
	{USB_DEVICE(HP_VID, HP_PID)},
	{}	
};

static int usbdev_probe(struct usb_interface *interface, const struct usb_device_id *id)
{
	int i;
	unsigned char epAddr, epAttr;
        unsigned char endpoint_in = 0, endpoint_out = 0;
	struct usb_device *device;
	struct usb_endpoint_descriptor *ep_desc;
	
        device = interface_to_usbdev(interface);

	if((id->idProduct == SAN_PID)&(id->idVendor == SAN_VID))
	{
		printk(KERN_INFO "Known SanDisk USB drive detected.\n");
	}
	else if((id->idProduct == HP_PID)&(id->idVendor == HP_VID))
	{
		printk(KERN_INFO "Known HP USB drive detected.\n");
	}
	else{
		printk(KERN_INFO "Unknown USB drive detected.\n");
	}
	
	printk(KERN_INFO "VID = %d \n",id->idVendor);
	printk(KERN_INFO "PID = %d \n",id->idProduct);

	printk(KERN_INFO "USB DEVICE CLASS : %x", interface->cur_altsetting->desc.bInterfaceClass);
	printk(KERN_INFO "USB DEVICE SUB CLASS : %x", interface->cur_altsetting->desc.bInterfaceSubClass);
	printk(KERN_INFO "USB DEVICE Protocol : %x", interface->cur_altsetting->desc.bInterfaceProtocol);

	printk(KERN_INFO "No. of Altsettings = %d\n",interface->num_altsetting);
	printk(KERN_INFO "No. of Endpoints = %d\n", interface->cur_altsetting->desc.bNumEndpoints);

	for(i=0;i<interface->cur_altsetting->desc.bNumEndpoints;i++)
	{
		ep_desc = &interface->cur_altsetting->endpoint[i].desc;
		epAddr = ep_desc->bEndpointAddress;
		epAttr = ep_desc->bmAttributes;
	
		if((epAttr & USB_ENDPOINT_XFERTYPE_MASK)==USB_ENDPOINT_XFER_BULK)
		{
			if(epAddr & 0x80){
				endpoint_in = ep_desc->bEndpointAddress;
				printk(KERN_INFO "EP %d is Bulk IN - %d \n", i, endpoint_in);
			}
			else{
				endpoint_out = ep_desc->bEndpointAddress;
				printk(KERN_INFO "EP %d is Bulk OUT - %d \n", i, endpoint_out);
			}
		}
	}

	if((interface->cur_altsetting->desc.bInterfaceClass == 0x08)&((interface->cur_altsetting->desc.bInterfaceSubClass == 0x01)|(interface->cur_altsetting->desc.bInterfaceSubClass == 0x06))&(interface->cur_altsetting->desc.bInterfaceProtocol == 0x50)){
		printk(KERN_INFO "Connected device is a USB Mass Storage - SCSI device");
		if(read_capacity(device,endpoint_in,endpoint_out)!=0)
		{ 
			printk(KERN_INFO"usbdev_probe : Error in Read Capacity \n");
			return -1;
		}
	}
	else{
		printk(KERN_INFO "Connected device doesnot support SCSI commands");
	}

return 0;
}

int read_capacity(struct usb_device *device, uint8_t endpoint_in, uint8_t endpoint_out)
{
	int ret, size,read_ep;
	uint8_t lun=0;
	uint32_t expected_tag;
	long max_lba, block_size;
	long device_size;
	uint8_t cdb[16];
	uint8_t *buffer=NULL;
	

	if ( !(buffer = (uint8_t *)kmalloc(sizeof(uint8_t)*64, GFP_KERNEL)) ) {
		printk(KERN_INFO"read_capacity : Cannot allocate memory for receive buffer\n");
		return -1;
	}

	
	memset(cdb, 0, sizeof(cdb));
	cdb[0] = 0x25;

	ret = usb_control_msg(device,usb_sndctrlpipe(device,0),BOMS_RESET,BOMS_RESET_REQ_TYPE,0,0,NULL,0,1000);
	if(ret < 0)
		printk(KERN_INFO "read_capacity : Cannot reset\n");
	else
		printk(KERN_INFO "read_capacity : Reset done\n");

	if( send_mass_storage_command(device,endpoint_out,lun,cdb,USB_EP_IN,READ_CAPACITY_LENGTH,&expected_tag) != 0 ) {
		printk(KERN_INFO"read_capacity : Send command error\n");
		return -1;
	}

	read_ep=usb_bulk_msg(device, usb_rcvbulkpipe(device,endpoint_in), (unsigned char*)buffer, READ_CAPACITY_LENGTH, &size, 1000);
	
	printk(KERN_INFO"read_capacity : Return value = %d\n",read_ep);
	
	printk(KERN_INFO"read_capacity : Received %d bytes\n", size);

	max_lba = be_to_int32(buffer);
	block_size = be_to_int32(buffer+4);
	device_size = (max_lba+1)*block_size/(1024*1024*1024);

	printk(KERN_INFO "Device Size: %ld GB\n",device_size);
	kfree(buffer);

	get_mass_storage_status(device, endpoint_in,expected_tag);

	return 0;
}

int send_mass_storage_command(struct usb_device *device, uint8_t endpoint_out, uint8_t lun,uint8_t *cdb, uint8_t direction, int data_length, uint32_t *ret_tag)  

{
	static uint32_t tag = 1;
	uint8_t cdb_len;
	int i, ret, size;
	struct command_block_wrapper *cbw;
	cbw = (struct command_block_wrapper*)kmalloc(sizeof(struct command_block_wrapper), GFP_KERNEL);
	
	if ( cbw == NULL ) {
		printk(KERN_INFO "send_mass_storage_command : Cannot allocate memory\n");
		return -1;}

	if (cdb == NULL) {
		return -1;
	}
	if (endpoint_out & USB_EP_IN) 
	{
		printk(KERN_INFO "send_mass_storage_command : Cannot send command on IN-EndPoint\n") ;
		return -1;
	}

	cdb_len = cdb_length[cdb[0]];

	if ((cdb_len == 0) || (cdb_len > sizeof(cbw -> CBWCB))) 
	{
		printk(KERN_INFO "send_mass_storage_command : Cannot handle this command (%02X, length %d)\n", cdb[0], cdb_len);
		return -1;
	}
	cdb_len = cdb_length[cdb[0]];
	
	memset(cbw,'\0',sizeof(struct command_block_wrapper));
	cbw->dCBWSignature[0] = 'U';
	cbw->dCBWSignature[1] = 'S';
	cbw->dCBWSignature[2] = 'B';
	cbw->dCBWSignature[3] = 'C';
	*ret_tag = tag;
	cbw->dCBWTag = tag++;
	cbw->dCBWDataTransferLength = data_length;
	cbw->bmCBWFlags = direction;
	cbw->bCBWLUN = lun;
	cbw->bCBWCBLength = cdb_len;
	memcpy(cbw->CBWCB, cdb, cdb_len);

	i = 0;
	do {
		// The transfer length must always be exactly 31 bytes.
		ret = usb_bulk_msg(device,usb_sndbulkpipe(device,endpoint_out), (unsigned char*)cbw, 31, &size, 1000);
		
		if (ret != 0) 
		{
			usb_clear_halt(device, usb_sndbulkpipe(device,endpoint_out));
		}
		else{
			break;
		}
		i++;
	} while (i<RETRY_MAX);
	
	if (ret != 0) {
		printk(KERN_INFO"send_mass_storage_command : Error in Send EndPoint Command\n");
		return -1;
	}
	
	printk(KERN_INFO"send_mass_storage_command : Succesfully sent %d CDB bytes\n", cdb_len);

	kfree(cbw);
	return 0;
}


int get_mass_storage_status(struct usb_device *device, uint8_t endpoint_in, uint32_t expected_tag) 
{
	int i, ret, size;
	struct command_status_wrapper *csw;

	if( !(csw = (struct command_status_wrapper*)kmalloc(sizeof(struct command_status_wrapper), GFP_KERNEL)) ) 
	{
		printk(KERN_INFO "get_mass_storage_status : Cannot allocate memory for command status buffer\n");
		return -1;
	}
	
	i = 0;
	do{
		ret = usb_bulk_msg(device, usb_rcvbulkpipe(device,endpoint_in), (unsigned char*)csw, 13, &size, 1000);
		if (ret != 0){
			 usb_clear_halt(device,usb_sndbulkpipe(device,endpoint_in));
			  }
		i++;
	   } while((ret!=0) && (i<5));

	if (ret != 0) 
	{
		printk(KERN_INFO "get_mass_storage_status : Error in Receive EndPoint Command\n");
		return -1;
	}

	if (size != 13) 
	{
		printk(KERN_INFO "get_mass_storage_status: Received %d bytes (expected 13)\n", size);
		return -1;
	}

	if (csw->dCSWTag != expected_tag) 
	{
		printk(KERN_INFO "get_mass_storage_status: Mismatched tags (expected %08X, received %08X)\n",expected_tag, csw->dCSWTag);
		return -1;
	}

	printk(KERN_INFO "get_mass_storage_status: Received CSW having status %d\n", csw->bCSWStatus);

	kfree(csw);

	return 0;	
	
}


/*Operations structure*/
static struct usb_driver usbdev_driver = {
	name: "usbdev",  //name of the device
	probe: usbdev_probe, // Whenever Device is plugged in
	disconnect: usbdev_disconnect, // When we remove a device
	id_table: usbdev_table, //  List of devices served by this driver
};


int device_init(void)
{
	usb_register(&usbdev_driver);
	return 0;
}

void device_exit(void)
{
	usb_deregister(&usbdev_driver);
}

module_init(device_init);
module_exit(device_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("RAJESH REDDY");
MODULE_DESCRIPTION("ASSIGNMENT-2");
