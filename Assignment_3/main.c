#include<linux/kernel.h>  
#include<linux/module.h>  
#include<linux/usb.h>    
#include <linux/slab.h>   // For kmalloc
#include<linux/types.h>   
#include <linux/fs.h>     // For file operation request
#include <linux/genhd.h>  // for gendisck structure
#include <linux/blkdev.h>  
#include <linux/errno.h>   
#include<linux/workqueue.h>  // for work queue creation
#include<linux/sched.h>   // for scheduling bottom half
#include<linux/delay.h>
#include<linux/bio.h>


#define SAN_VID  0x058f
#define SAN_PID  0x6387

#define HP_VID  0x03f0
#define HP_PID  0x9140

#define GET_MAX_LUN        0xFE    
#define REQ_TYPE		   0xA1      //request type
#define RESET_TYPE         0x21    //reset type
#define RESET_D			   0xFF   

#define READ_CAPACITY_LENGTH          0x08  // byte number for capacity command
#define READ_INQ_LENGTH          0x24     // byte number for Inquiry command
#define READ_DATA               0X200     

#define CBW_LEN 31   
#define CSW_LEN 13   


#define bio_iovec_idx(bio, idx)	(&((bio)->bi_io_vec[(idx)]))
#define __bio_kmap_atomic(bio, idx, kmtype)				\
	(kmap_atomic(bio_iovec_idx((bio), (idx))->bv_page) +	\
		bio_iovec_idx((bio), (idx))->bv_offset)
#define __bio_kunmap_atomic(addr, kmtype) kunmap_atomic(addr)

unsigned int takenote=0;
static struct workqueue_struct *myqueue = NULL; 

/* structure for workqueue*/
struct my_work{
	struct work_struct work;
	void *data;
}; 
 

static struct rb_device
{
	
	unsigned int size;
	spinlock_t lock;
	struct request_queue *rb_queue;
	struct gendisk *rb_disk;
} rb_dev;


static u32 tag = 1;       //generate unique tag
static u_int rb_major = 0;     //major number of device
static u32  max_lba,block_size1;  //logical block adress and block size
u8 lun;   
u8 cdb[16]; 
u32 exp_tag; 
u8 endpoint_in_addr,endpoint_out_addr; //endpoint adress

struct usb_device *udev; 

static u8 cdb_length[256] = {
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

//supported device table
static struct usb_device_id usbdev_table [] = {
	{USB_DEVICE(SAN_VID, SAN_PID)},
	{USB_DEVICE(HP_VID, HP_PID)},
	{} 	
};


// Command Block Wrapper (CBW)
struct command_block_wrapper {
	u8 dCBWSignature[4];
	u32 dCBWTag;
	u32 dCBWDataTransferLength;
	u8 bmCBWFlags;
	u8 bCBWLUN;
	u8 bCBWCBLength;
	u8 CBWCB[16];
};


// Command Status Wrapper (CSW)
struct command_status_wrapper {
	u8 dCSWSignature[4];
	u32 dCSWTag;
	u32 dCSWDataResidue;
	u8 bCSWStatus;
};


//function prototype
int find_lun(void);
int only_inq(void);
int only_cap(void);
int only_read(sector_t sector_off, u8 *buffer, unsigned int sectors);
int only_write(sector_t sector_off, u8 *buffer, unsigned int sectors);
int get_status(void);
int reset_dev(void);
int initblock(void);


//function defenisions
int get_status()
{	
	int size,k;
	struct command_status_wrapper *csw=NULL;
	csw=(struct command_status_wrapper*)kmalloc(sizeof (struct command_status_wrapper),GFP_DMA) ;
	printk(KERN_INFO "checking status\n");
	
	k=usb_bulk_msg(udev, usb_rcvbulkpipe (udev,endpoint_in_addr), (unsigned char*)csw, CSW_LEN, &size, 3000);   //recieve the status
	if(k!=0)
	{
		return k;
		kfree(csw);
	}
	if(csw->bCSWStatus!=0)
	{
		printk(KERN_INFO "	status: failed\n");
		kfree(csw);
		return -1;
	}
	if(csw->dCSWTag!=exp_tag) 
	{
		printk(KERN_INFO "tag not matched\n");
		kfree(csw);
		return -1;
	}
	if(csw->dCSWSignature[0]!=85 || csw->dCSWSignature[1]!=83 || csw->dCSWSignature[2]!= 66 || csw->dCSWSignature[3]!=83)
	{
		printk(KERN_INFO "	signalture not matched\n");
		kfree(csw);
		return -1;
	}
	printk(KERN_INFO "   status sucess\n");
	printk("   tag=%u, status=%u, signature=%x%x%x%x\n",csw->dCSWTag,csw->bCSWStatus,csw->dCSWSignature[0],csw->dCSWSignature[1],csw->dCSWSignature[2],csw->dCSWSignature[3]);
	kfree(csw);
	return 0;

}

int reset_dev(){
int r;   //number of bytes receivedd after control msg sent or error
void *lun1= kmalloc(1, GFP_KERNEL); //to store recived bytes indication after resetting

printk(KERN_INFO "sending control messege to reset\n");
r=usb_control_msg(udev, usb_rcvctrlpipe (udev,0x00),RESET_D,RESET_TYPE, 0, 0, lun1, 0, 10000);  //sending control signal
memcpy(&lun, lun1, 1);
kfree(lun1);

if(r==0)   //for stall
{
lun=0;
}

if(r<0)  //for error
{
return r;
}

printk("   received byte after control signal sent=%d, Max lun got as: %u \n",r,lun);  


return 0;
}

int find_lun()
{
int r;   
void *lun1= kmalloc(1, GFP_KERNEL);

printk(KERN_INFO "sending control messege to get max LUN\n");
r=usb_control_msg(udev, usb_rcvctrlpipe (udev,0x00),GET_MAX_LUN,REQ_TYPE, 0, 0, lun1, 1, 10000);  //sending control signal
memcpy(&lun, lun1, 1);
kfree(lun1);

if(r==0)   //for stall
{
lun=0;
}

if(r<0)  //for error
{
return r;
}

printk("   received byte after control signal sent=%d, Max lun got as: %u \n",r,lun);  

return 0;
}

int only_inq()
{
	char *buffer = kmalloc(64, GFP_DMA);  //buffer to store inquiry data
	unsigned char vid[9], pid[9], rev[5];            // veryfy PID and VID through SCSI command
	int ei,ej;        
	int i,k;			
	u8 cdb_len;          //CBWCB length
	int size; 



	struct command_block_wrapper *cbw=NULL;
	cbw=(struct command_block_wrapper*)kmalloc(sizeof (struct command_block_wrapper),GFP_DMA) ;
	memset(cdb, 0, sizeof(cdb));
	
   	cbw->dCBWSignature[0] = 'U';
	cbw->dCBWSignature[1] = 'S';
	cbw->dCBWSignature[2] = 'B';
	cbw->dCBWSignature[3] = 'C';	
	cbw->bCBWLUN = lun;
	cbw->bmCBWFlags = USB_DIR_IN; //since data transfer is 'in' for both data Inquiry and capacity- 0x80

	cdb[0] = 0x12;	// Inquiry
	cdb[4] = 0x24;
	cdb_len = cdb_length[cdb[0]];
	cbw->dCBWTag = tag;
	exp_tag=tag;
	cbw->dCBWDataTransferLength = READ_INQ_LENGTH; //
 	cbw->bCBWCBLength = cdb_len;
	memcpy(cbw->CBWCB, cdb, cdb_len);
	exp_tag=tag;
	
	printk(KERN_INFO "Sending inquiry command\n");
	ei=usb_bulk_msg(udev, usb_sndbulkpipe(udev,endpoint_out_addr),(unsigned char*)cbw, 31, &size, 10000);
	if(ei!=0)
	{
	return ei;
	}
	printk(KERN_INFO "   sent %d CDB bytes and sent %d CBW bytes\n", cdb_len,size);
	
	ej=usb_bulk_msg(udev, usb_rcvbulkpipe(udev,endpoint_in_addr), (unsigned char*)buffer, READ_INQ_LENGTH, &size, 1000);
	printk(KERN_ALERT "   received inquiry %d bytes\n", size);
	if(ej!=0)
	{
	return ej;
	}
	
	k=get_status();
	if(k==-1)
	{
		return -1;
	}

	for (i=0; i<8; i++) {
		vid[i] = *(buffer+8+i);
		pid[i] = *(buffer+16+i);
		rev[i/2] = *(buffer+32+i/2);	// instead of another loop
	}

	vid[8] = 0;
	pid[8] = 0;
	rev[4] = 0;
	printk(KERN_ALERT "	after inquiry   VID:PID:REV \"%8s\":\"%8s\":\"%4s\"\n", vid, pid, rev); 
	
	return 0;
}

int only_cap()
{
	char *buffer = kmalloc(8, GFP_DMA);
	unsigned int jj;
	uint8_t buffer1[8];  //buffer to store capacity data
	u64 big,temp1,temp2,temp3;  //for capacity calculation
	int i,r,k;			 //error codes for: bulk send , bulk recieve, stauts
	int size,size1; 
	u8 cdb_len;          //CBWCB length 
	//static u32 tag = 2;
	int ct=0;   //incase of stall 
	struct command_block_wrapper *cbw=NULL;
	cbw=(struct command_block_wrapper*)kmalloc(sizeof (struct command_block_wrapper),GFP_DMA) ;
	memset(cdb, 0, sizeof(cdb));
	
	cbw->dCBWSignature[0] = 'U';
	cbw->dCBWSignature[1] = 'S';
	cbw->dCBWSignature[2] = 'B';
	cbw->dCBWSignature[3] = 'C';	
	cbw->bCBWLUN = lun;
	cbw->bmCBWFlags = USB_DIR_IN; //since data transfer is in for both data Inquiry and capacity- 0x80

	cdb[0] = 0x25;
	tag++;
	cbw->dCBWTag = tag;
	exp_tag=tag;
	cbw->dCBWDataTransferLength = READ_CAPACITY_LENGTH; //
	cdb_len = cdb_length[cdb[0]];	
	cbw->bCBWCBLength = cdb_len;
	memcpy(cbw->CBWCB, cdb, cdb_len);

	printk(KERN_INFO "sending capacity read command\n");
	r = usb_bulk_msg(udev, usb_sndbulkpipe(udev,endpoint_out_addr),(unsigned char*)cbw, 31, &size, 10000);

   while(r!=0 && ct<5)  //if not sent first time
	{
		r=usb_clear_halt(udev,usb_sndbulkpipe(udev,endpoint_out_addr)) ;

		r = usb_bulk_msg(udev, usb_sndbulkpipe(udev,endpoint_out_addr),(unsigned char*)cbw, 31, &size, 1000);
		ct++;

	}
	if(ct==5 && r!=0)
	{
		ct=0;
		kfree(cbw);
		kfree(buffer);
		return r;
	}

	printk(KERN_INFO "   sent %d CDB bytes and sent %d CBW bytes\n", cdb_len,size);

	i=usb_bulk_msg(udev, usb_rcvbulkpipe (udev,endpoint_in_addr), (unsigned char*)buffer,  READ_CAPACITY_LENGTH, &size1, 10000);
	printk(KERN_INFO "   received %d bytes\n", size1);
	if(i!=0)
	{
		kfree(cbw);
		kfree(buffer);
		return i;
	}
	k=get_status();  //check status
	if(k==-1)
	{
		kfree(cbw);
		kfree(buffer);
		return -1;
	}
	for(jj=0;jj<8;jj++)
	buffer1[jj]=*(buffer+jj);
	 

	max_lba= (buffer1[0] << 24) | (buffer1[1] << 16) | ( buffer1[2] << 8 ) | (buffer1[3]);   //get max logical adress
	block_size1=(buffer1[4] << 24) | (buffer1[5] << 16) | ( buffer1[6] << 8 ) | (buffer1[7]); //get block size
	big=(u64)(max_lba+1)*(u64)(block_size1); //total memeory size in bytes
	temp1=big>>30;
	temp2=big%(1<<30);
	temp3=(temp2*10)>>30; //total memory size in GB
	printk(KERN_INFO "capacity of device is:\n");
	printk(KERN_INFO "	in hex:   Max LBA: %08X, Block Size: %08X \n	therefore total size: (%llx Bytes)\n", max_lba, block_size1, big);
	printk(KERN_INFO "	in decimal:   Max LBA: %u, Block Size: %u Bytes\n	therefore total size: (%lu Bytes) (%lu.%lu GB)\n", (unsigned int)max_lba,(unsigned int)block_size1,(unsigned long) big,(unsigned long)temp1,(unsigned long)temp3);
	kfree(cbw);
	kfree(buffer);
	return 0;

}

int only_read(sector_t sector_off, u8 *buffer, unsigned int sectors)
{
	unsigned int hi;         //for total bytes recived
	int i,r,k,r1;			 //error codes for: bulk send , bulk recieve, stauts
	int size2,size3;  
	u8 cdb_len;          //CBWCB length
	int ct=0;   //incase of stall 

	struct command_block_wrapper *cbw=NULL;
	cbw=(struct command_block_wrapper*)kmalloc(sizeof (struct command_block_wrapper),GFP_DMA) ;
	//memset(&cbw, 0, sizeof(cbw));
	memset(cdb, 0, sizeof(cdb));
	r=0;
	cbw->dCBWSignature[0] = 'U';
	cbw->dCBWSignature[1] = 'S';
	cbw->dCBWSignature[2] = 'B';
	cbw->dCBWSignature[3] = 'C';	
	cbw->bCBWLUN = lun;
	cbw->bmCBWFlags = USB_DIR_IN; //since data transfer is' in' for read- 0x80
	cdb[0] = 0x28;
	cdb[1] = 0x00;
	cdb[2] = (sector_off & 0xFF000000) >> 24;
	cdb[3] = (sector_off & 0xFF0000) >> 16;
	cdb[4] = (sector_off & 0xFF00) >> 8;
	cdb[5] = (sector_off & 0xFF);
	cdb[7] = (sectors & 0xff00);
	cdb[8] = (sectors & 0x00ff);
	tag++;
	cbw->dCBWTag = tag;
	exp_tag=tag;
	hi=READ_DATA*sectors;
	cbw->dCBWDataTransferLength =hi ; //
	cdb_len = cdb_length[cdb[0]];	
	cbw->bCBWCBLength = cdb_len;
	memcpy(cbw->CBWCB, cdb, cdb_len);
	r1 = usb_bulk_msg(udev, usb_sndbulkpipe(udev,endpoint_out_addr),(unsigned char*)cbw, 31, &size3, 1000000);  //send command to read
	   while(r1!=0 && ct<5)  //if not sent first time
	{
		r1=usb_clear_halt(udev,usb_sndbulkpipe(udev,endpoint_out_addr)) ;
		r1 = usb_bulk_msg(udev, usb_sndbulkpipe(udev,endpoint_out_addr),(unsigned char*)cbw, 31, &size3, 1000);
		ct++;

	}
	if(ct==5 && r!=0)
	{	ct=0;
		kfree(cbw);
		return r;
	}
	
	i=usb_bulk_msg(udev, usb_rcvbulkpipe (udev,endpoint_in_addr), (unsigned char*)buffer,  hi, &size2, 1000000);   //reading from device
	printk(KERN_INFO "   received %d bytes\n", size2);
	if(i!=0)
	{
		kfree(cbw);
		return i;
	}
	k=get_status();  //check status
	if(k==-1)
	{
		kfree(cbw);
		return -1;
	}
	kfree(cbw);
	return 0;

}

int only_write(sector_t sector_off, u8 *buffer, unsigned int sectors)
{
	
   unsigned int hello;  
	int i,r,k,r1;			 //error codes for: bulk send , bulk recieve, stauts 
	int size4,size5;  //received sizes
	u8 cdb_len;          //CBWCB length

	int ct=0;   //incase of stall 

	struct command_block_wrapper *cbw=NULL;
	cbw=(struct command_block_wrapper*)kmalloc(sizeof (struct command_block_wrapper),GFP_DMA) ;
	memset(cdb, 0, sizeof(cdb));
	
	r=0;
	cbw->dCBWSignature[0] = 'U';
	cbw->dCBWSignature[1] = 'S';
	cbw->dCBWSignature[2] = 'B';
	cbw->dCBWSignature[3] = 'C';	
	cbw->bCBWLUN = lun;
	cbw->bmCBWFlags = USB_DIR_OUT;	//since writing direction is out
	cdb[0] = 0x2a;
	cdb[1] = 0x00;
	cdb[2] = (sector_off & 0xFF000000) >> 24;
	cdb[3] = (sector_off & 0xFF0000) >> 16;
	cdb[4] = (sector_off & 0xFF00) >> 8;
	cdb[5] = (sector_off & 0xFF);
	cdb[7] = (sectors & 0xff00);
	cdb[8] = (sectors & 0x00ff);
	tag++;
	cbw->dCBWTag = tag;
	exp_tag=tag;
	hello=512*sectors;   //total bytes to recieve
	cbw->dCBWDataTransferLength = hello; 
	cdb_len = cdb_length[cdb[0]];	
	cbw->bCBWCBLength = cdb_len;
	memcpy(cbw->CBWCB, cdb, cdb_len);


   r1=usb_clear_halt(udev,usb_rcvbulkpipe(udev,endpoint_in_addr)) ;
	r1=usb_clear_halt(udev,usb_sndbulkpipe(udev,endpoint_out_addr)) ;	

	printk(KERN_INFO "sending DATA write command\n");
	r1 = usb_bulk_msg(udev, usb_sndbulkpipe(udev,endpoint_out_addr),(unsigned char*)cbw, 31, &size4, 1000000);
	   while(r1!=0 && ct<5)  //if not sent first time
	{
		r1=usb_clear_halt(udev,usb_sndbulkpipe(udev,endpoint_out_addr)) ;
		r1 = usb_bulk_msg(udev, usb_sndbulkpipe(udev,endpoint_out_addr),(unsigned char*)cbw, 31, &size4, 1000000);
		ct++;

	}
	if(ct==5 && r!=0)
	{	ct=0;
		kfree(cbw);
		return r;
	}

	i=usb_bulk_msg(udev, usb_sndbulkpipe (udev,endpoint_out_addr), (unsigned char*)buffer,  hello, &size5, 1000000);
	printk(KERN_INFO "   sent %d bytes\n", size5);
	if(i!=0)
	{
		kfree(cbw);
		return i;
	}
	
	r1=usb_clear_halt(udev,usb_sndbulkpipe(udev,endpoint_out_addr)) ;
	if(r1!=0)
	{
		kfree(cbw);
		return r1;
	}
	r1=usb_clear_halt(udev,usb_rcvbulkpipe(udev,endpoint_out_addr)) ;
	if(r1!=0)
	{
		kfree(cbw);
		return r1;
	}
	//getting the status of transaction
	k=get_status();
	if(k!=0)
	{
		kfree(cbw);
		return k;
	}
	kfree(cbw);
	return 0;



}

/* open function definition*/
static int rb_open(struct block_device *bdev, fmode_t mode)
{
	unsigned unit = iminor(bdev->bd_inode);
	
	printk(KERN_INFO "Pendrive: Device is opened\n");
	printk(KERN_INFO "Pendrive: Inode number is %d\n", unit);

	if (unit > 2)
		return -ENODEV;
	return 0;
}


/* close function definition*/
static void rb_close(struct gendisk *disk, fmode_t mode)

{
		
	printk(KERN_INFO "Pendrive: Device is closed\n");
	
}

static void rb_transfer(struct work_struct *work)
{
	
	struct my_work *mwp = container_of(work, struct my_work, work);   //to point to structure itself
	struct request *req=mwp->data;        
	int dir = rq_data_dir(req);       
	sector_t start_sector = blk_rq_pos(req);    //starting sector 
	unsigned int sector_cnt = blk_rq_sectors(req); //number of sector to pass

	struct bio_vec bv;     
	struct req_iterator iter;  

	sector_t sector_offset; 
	unsigned int sectors;    
	u8 *buffer;   

	int ret = 0;
	int er;
	printk(KERN_DEBUG "Pendrive: Dir:%d; Sec:%lld; Cnt:%d\n", dir, (long long int)start_sector, (int)sector_cnt); 

	sector_offset = 0;
	//for each  segment in request
	rq_for_each_segment(bv, req, iter)
	{
		buffer = page_address(bv.bv_page) + bv.bv_offset;  //pointes where to read and write in ram
		if (bv.bv_len % 512 != 0)       
		{
			printk(KERN_ERR "Pendrive: Should never happen: "
				"bio size (%d) is not a multiple of RB_SECTOR_SIZE (%d).\n"
				"This may lead to data truncation.\n",
				bv.bv_len, 512);
			ret = -EIO;
		}
		sectors = bv.bv_len / 512;  //to get number of sectors to 
		printk(KERN_DEBUG "Pendrive: Sector Offset: %lld; Buffer: %p; Length: %d sectors\n",(long long int)sector_offset, buffer, sectors);
		if (dir == WRITE) /* Write to the device */
		{
			
			er=only_write(start_sector, buffer, sectors);
			printk(KERN_INFO "error inside write block %d\n",er);
		}
		else /* Read from the device */
		{
			er=only_read(start_sector, buffer, sectors);
			printk(KERN_INFO "error inside READ block %d\n",er);
		}
		sector_offset += sectors;
	}
	if (sector_offset != sector_cnt)
	{
		printk(KERN_ERR "Pendrive: bio info doesn't match with the request info");
		ret = -EIO;
	}
	
	__blk_end_request_all(req, ret);   //gives back the status of the transaction
	if(takenote==1000) {
		takenote=0;
		msleep(1000); }
 	else
	takenote+=1;
}

static void rb_request(struct request_queue *q)
{
	struct request *req;
	
	struct my_work *working=NULL;
	/* Gets the current request from the dispatch queue */

	while ((req = blk_fetch_request(q)) != NULL)
	{
	working=(struct my_work*)kmalloc(sizeof (struct my_work),GFP_DMA) ;
			if(working==NULL)
						{

					__blk_end_request_all(req, 0);
						printk(KERN_INFO "no memo alocation\n");
						continue;

						}
			working->data = req;   //store arguement for accessing in bottom half function
			INIT_WORK(&working->work, rb_transfer);   //initialize work at runtime
			queue_work(myqueue, &working->work);    //schedle the work in local work queue
		   
		
	}
	
}

static struct block_device_operations rb_fops =
{
	.owner = THIS_MODULE,
	.open = rb_open,
	.release = rb_close,
};



int initblock()
{
	  //maximum logical block adress and block size
	rb_dev.size =max_lba+1; //---------------------see

	/* Get Registered */
	rb_major = register_blkdev(rb_major, "Pendrive");
	if (rb_major <= 0)
	{
		printk(KERN_ERR "rb: Unable to get Major Number\n");
		return -EBUSY;
	}

	/* Get a request queue (here queue is created) */
	spin_lock_init(&rb_dev.lock);
	rb_dev.rb_queue = blk_init_queue(rb_request, &rb_dev.lock);
	if (rb_dev.rb_queue == NULL)
	{
		printk(KERN_ERR "rb: blk_init_queue failure\n");
		unregister_blkdev(rb_major, "Pendrive");
		return -ENOMEM;
	}
	
	
	rb_dev.rb_disk = alloc_disk(2);                
	if (!rb_dev.rb_disk)
	{
		printk(KERN_ERR "Pendrive: alloc_disk failure\n");
		blk_cleanup_queue(rb_dev.rb_queue);
		unregister_blkdev(rb_major, "Pendrive");
		return -ENOMEM;
	}
	myqueue = create_workqueue("myqueue"); //create a local work queue
	

 	/* Setting the major number */
	rb_dev.rb_disk->major = rb_major;
  	/* Setting the first mior number */
	rb_dev.rb_disk->first_minor = 0;
 	/* Initializing the device operations */
	rb_dev.rb_disk->fops = &rb_fops;
 	/* Driver-specific own internal data */
	rb_dev.rb_disk->private_data = &rb_dev;
	rb_dev.rb_disk->queue = rb_dev.rb_queue;
	
	sprintf(rb_dev.rb_disk->disk_name, "Pendrive");
	/* Setting the capacity of the device in its gendisk structure */
	set_capacity(rb_dev.rb_disk, rb_dev.size);

	/* Adding the disk to the system */
	add_disk(rb_dev.rb_disk);
	/* Now the disk is "live" */
	printk(KERN_INFO "Pendrive: Ram Block driver initialised (%d sectors; %d bytes)\n",
		rb_dev.size, rb_dev.size * block_size1);

	return 0;




}



static void usbdev_disconnect(struct usb_interface *interface)
{
	printk(KERN_INFO "USBDEV Device Removed\n");
	flush_workqueue(myqueue);
	destroy_workqueue(myqueue);
	del_gendisk(rb_dev.rb_disk);
	put_disk(rb_dev.rb_disk);
	blk_cleanup_queue(rb_dev.rb_queue);
	unregister_blkdev(rb_major, "Pendrive");
	spin_unlock(&rb_dev.lock);      
	return;
}


static int usbdev_probe(struct usb_interface *interface, const struct usb_device_id *id)
{
	int i;
	int err1;
	u8 epAddr, epAttr;  //for end point adress and end point attribute
	struct usb_endpoint_descriptor *ep_desc;
	udev = interface_to_usbdev(interface);
	
	if(id->idProduct == HP_PID)
	{
		printk(KERN_INFO "Known HP USB drive detected\n");
	}
	else if(id->idProduct == SAN_PID)
	{
		printk(KERN_INFO "Known SANDISK USB drive detected\n");
	}
	else
	{
		printk(KERN_INFO "UNnown USB drive detected\n");
	}

	printk(KERN_INFO "USB DEVICE VID : %x\n", id->idVendor);
	printk(KERN_INFO "USB DEVICE PID : %x\n", id->idProduct);
	printk(KERN_INFO "USB DEVICE CLASS : %x\n", interface->cur_altsetting->desc.bInterfaceClass);
	printk(KERN_INFO "USB DEVICE SUB CLASS : %x\n", interface->cur_altsetting->desc.bInterfaceSubClass);
	printk(KERN_INFO "USB DEVICE Protocol : %x\n", interface->cur_altsetting->desc.bInterfaceProtocol);
	printk(KERN_INFO "No. of Endpoints = %d\n", interface->cur_altsetting->desc.bNumEndpoints);
	printk(KERN_INFO "No. of Altsettings = %d\n",interface->num_altsetting);

	for(i=0;i<interface->cur_altsetting->desc.bNumEndpoints;i++)
	{
		ep_desc = &interface->cur_altsetting->endpoint[i].desc;
		epAddr = ep_desc->bEndpointAddress;
		epAttr = ep_desc->bmAttributes;
	
		if((epAttr & USB_ENDPOINT_XFERTYPE_MASK)==USB_ENDPOINT_XFER_BULK)
		{
			if(epAddr & USB_DIR_IN){
				endpoint_in_addr=epAddr;
				printk(KERN_INFO "EP %d is Bulk IN address=%x \n", i,endpoint_in_addr); 
				
		
				}
			else{
				endpoint_out_addr=epAddr;
				printk(KERN_INFO "EP %d is Bulk OUT address=%x\n", i, endpoint_out_addr);
				
				}
	
		}

	}

	
	printk(KERN_INFO "resetting device\n");
	reset_dev();


	err1=find_lun();  //to find max lun of device
	if(err1!=0)
	{
		 return err1;
	}
	//err1=only_inq();	//to inquiry the device about device specification such as VID PID etc
	if(err1!=0)
	{
		 return err1;
	}
	max_lba=30277632;
	err1=only_cap();	//to find capacity of device
	if(err1!=0)
	{
		 return err1;
	}
	
	err1=initblock(); //to initialize the usb device as block device
	if(err1!=0)
	{
		 return err1;
	}
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
	int res;
	
	res=usb_register(&usbdev_driver);
	if(res!=0)
	{
		printk("usb_register failed. Error number %d\n", res);
		return res;
	}
	printk(KERN_INFO "UAS READ Capacity Driver Inserted\n");
	return 0;
}

void device_exit(void)
{
	usb_deregister(&usbdev_driver);
	printk(KERN_NOTICE "Leaving USB Device Kernel\n");

	
}

module_init(device_init);
module_exit(device_exit);
MODULE_LICENSE("GPL");


ppp1.c
Displaying ppp1.c.
