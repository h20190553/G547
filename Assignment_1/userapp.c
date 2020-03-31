#include<stdio.h>
#include<stdlib.h>
#include<unistd.h>
#include<string.h>
#include<fcntl.h>
#include<linux/ioctl.h>


#define MAGIC_NUMBER 'A'
#define CHANNEL_SELECT_SEQNO 1
#define RESULT_ALIGNMENT_SEQNO 2

#define CHANNEL_SELECT _IOW(MAGIC_NUMBER, CHANNEL_SELECT_SEQNO, unsigned long)
#define RESULT_ALIGNMENT _IOW(MAGIC_NUMBER, RESULT_ALIGNMENT_SEQNO, unsigned long)


unsigned int channel;
unsigned int align;
unsigned int result;

int main(int arg,char *files[])
{
	int devicefile, userfile;
	if(arg != 2)
	{
		printf("Please pass the file name...\n");
		exit(1);
	}

	devicefile = open(files[1], O_RDONLY);

	if(devicefile == -1)
	{
		printf("Cannot open source file\n");
		exit(1);
	}

	printf("Enter the Required Channel number of ADC(0-7) : \n");
        scanf("%d",&channel);       
        ioctl(devicefile, CHANNEL_SELECT, &channel); 
 
        printf("Enter the Required Allignment(0/1) : \n");
	scanf("%d",&align); 
        ioctl(devicefile, RESULT_ALIGNMENT, &align);
       
 
        printf("Reading value from ADC...\n");
	read(devicefile, &result, 2);
	printf("Value read from ADC - %d \n", result);
        close(devicefile);
	exit(0);
}

