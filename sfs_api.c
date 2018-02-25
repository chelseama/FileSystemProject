
#include "sfs_api.h"
#include "bitmap.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fuse.h>
#include <strings.h>
#include "disk_emu.h"

//#define PRINT_ERRORS
//#define PRINT_FN_CALLS
//#define PRINT_SFS_FOPEN
//#define PRINT_SFS_FREAD
//#define PRINT_SFS_FWRITE

#define CEILING(num, denom) ((num % denom == 0) ? num/denom : num/denom+1)
#define FLOOR(num, denom) (num/denom)

#define LASTNAME_FIRSTNAME_DISK "sfs_disk.disk"
#define NUM_BLOCKS 1024  //maximum number of data blocks on the disk.
#define NUM_INODES 100	//max number of inodes
#define BLOCK_SIZE 1024

#define NUM_BLOCKS_SUPERBLOCK  1
#define NUM_BLOCKS_INODET      8
#define NUM_BLOCKS_FREE_BITMAP 1
#define BLOCK_INDEX_SUPERBLOCK     0
#define BLOCK_INDEX_INODET        (BLOCK_INDEX_SUPERBLOCK+NUM_BLOCKS_SUPERBLOCK)
#define BLOCK_INDEX_DATA_BLOCKS   (BLOCK_INDEX_INODET+NUM_BLOCKS_INODET)
#define BLOCK_INDEX_FREE_BITMAP   (BLOCK_INDEX_DATA_BLOCKS+NUM_BLOCKS)
#define NUM_TOTAL_BLOCKS (NUM_BLOCKS_SUPERBLOCK+NUM_BLOCKS_INODET+NUM_BLOCKS+NUM_BLOCKS_FREE_BITMAP)

#define FREE_BM_SIZE (130) // ceiling of NUM_TOTAL_BLOCKS/8
#define INODE_TABLE_BM_SIZE (13) // ceiling of num inodes/8
#define FD_TABLE_BM_SIZE (13)     // ceiling of num inodes/8
#define DIR_ENTRIES_BM_SIZE (13) //ceiling of num inodes/8

file_descriptor fd_table[NUM_INODES];
inode_t inode_table[NUM_INODES];
directory_entry rootDir[NUM_INODES];
superblock_t super_block;
int inodeIndexForRootDir=0;
int dirEntryIndexForRootDir=0;
int dirEntryTrackerIndex;

//initialize all bitmaps to high
uint8_t free_bit_map[FREE_BM_SIZE] = { [0 ... FREE_BM_SIZE - 1] = UINT8_MAX };
uint8_t inode_table_bit_map[INODE_TABLE_BM_SIZE] = { [0 ... INODE_TABLE_BM_SIZE - 1] = UINT8_MAX };
uint8_t dir_entries_bit_map[DIR_ENTRIES_BM_SIZE] = { [0 ... DIR_ENTRIES_BM_SIZE - 1] = UINT8_MAX };
uint8_t fd_table_bit_map[FD_TABLE_BM_SIZE] = { [0 ... FD_TABLE_BM_SIZE - 1] = UINT8_MAX };

void init_free_bm() {
	memset(free_bit_map, UINT8_MAX, FREE_BM_SIZE);

	force_set_index(free_bit_map, BLOCK_INDEX_FREE_BITMAP);
}

void init_fdt() {
	for(int i=0; i<NUM_INODES; i++) {
		fd_table[i].inode = NULL;
		fd_table[i].inodeIndex = -1;
		fd_table[i].rwptr = 0;
	}
	// Initialize bitmap for file descriptor table
	memset(fd_table_bit_map, UINT8_MAX, FD_TABLE_BM_SIZE);
	
	dirEntryTrackerIndex=0;
}

void init_inodet() {
	for(int i=0; i<NUM_INODES; i++) {
		inode_table[i].mode = -1;
		inode_table[i].link_cnt = -1;
		inode_table[i].uid = -1;
		inode_table[i].gid = -1;
		inode_table[i].size = -1;
		inode_table[i].indirectPointer = -1;
		for(int j=0; j<12; j++) {
			inode_table[i].data_ptrs[j] = -1;
		}
	}
	// Initialize bitmap for inode table
	memset(inode_table_bit_map, UINT8_MAX, INODE_TABLE_BM_SIZE);

	for (int i = 0; i < NUM_BLOCKS_INODET; ++i) {
		force_set_index(free_bit_map, BLOCK_INDEX_INODET+i);
	}
}

void init_super(){
	super_block.magic = 0xACBD0005;
	super_block.block_size = BLOCK_SIZE;
	super_block.fs_size = NUM_TOTAL_BLOCKS;
	super_block.inode_table_len = 0;
	super_block.root_dir_inode = 0;

	force_set_index(free_bit_map, BLOCK_INDEX_SUPERBLOCK);
}

void init_rootDir(){
	// Get free index from inode table bitmap
	int inodeIndexForRootDir = get_index(inode_table_bit_map);

	// Write inode entry for rootDir
	inode_table[inodeIndexForRootDir].size = 0; // assume directory has 0 size
	inode_table[inodeIndexForRootDir].data_ptrs[0] = get_index(free_bit_map);
	inode_table[inodeIndexForRootDir].data_ptrs[1] = get_index(free_bit_map);
	inode_table[inodeIndexForRootDir].data_ptrs[2] = get_index(free_bit_map);
	
	// Initialize bit map for dir entry
	memset(dir_entries_bit_map, UINT8_MAX, DIR_ENTRIES_BM_SIZE);
	
	// Initialize directory entries 
	for(int i=0; i<NUM_INODES; i++) {
		rootDir[i].num = -1;
		for(int j=0; j<MAX_FILE_NAME; j++) {
			rootDir[i].name[j]= '\0';
		}
	}
	
	// DO NOT PUT ROOTDIR in LIST OF ROOTDIR ENTRIES: Occupy bit map for root dir
	//dirEntryIndexForRootDir = get_index(dir_entries_bit_map);
	//rootDir[dirEntryIndexForRootDir].num = inodeIndexForRootDir;
	//rootDir[dirEntryIndexForRootDir].name[0] = '/';	
}

void write_superblock_to_disk() {
	// Initialize buffer
	char buffer[BLOCK_SIZE];
	memset(buffer, 0, BLOCK_SIZE);
	
	// Put super_block into buffer (need 1 block)
	memcpy(buffer, &super_block, sizeof(superblock_t));
	
	// Write block to disk
	write_blocks(BLOCK_INDEX_SUPERBLOCK, 1, buffer);
}

void write_rootDir_to_disk() {
	// Initialize buffer
	char buffer[3*BLOCK_SIZE];
	memset(buffer, 0, 3*BLOCK_SIZE);
	
	// Put rootDir and dir_entries_bit_map into buffer (need 3 blocks)
	unsigned int rootDir_num_bytes = NUM_INODES*sizeof(directory_entry);
	unsigned int dir_entries_bm_num_bytes = DIR_ENTRIES_BM_SIZE*sizeof(uint8_t);
	memcpy(buffer+0, rootDir, rootDir_num_bytes);
	memcpy(buffer+rootDir_num_bytes, dir_entries_bit_map, dir_entries_bm_num_bytes);
	
	// Write buffer blocks to disk
	write_blocks(inode_table[inodeIndexForRootDir].data_ptrs[0], 1, buffer+0);
	write_blocks(inode_table[inodeIndexForRootDir].data_ptrs[1], 1, buffer+BLOCK_SIZE);
	write_blocks(inode_table[inodeIndexForRootDir].data_ptrs[2], 1, buffer+2*BLOCK_SIZE);
	
}

void write_inodet_to_disk() {
	// Initialize buffer
	char buffer[8*BLOCK_SIZE];
	memset(buffer, 0, 8*BLOCK_SIZE);
	
	// Put inode_table and inode_table_bit_map into buffer (need 8 blocks)
	unsigned int inode_table_num_bytes = NUM_INODES*sizeof(inode_t);
	unsigned int inode_table_bm_num_bytes = INODE_TABLE_BM_SIZE*sizeof(uint8_t);
	memcpy(buffer+0, inode_table, inode_table_num_bytes);
	memcpy(buffer+inode_table_num_bytes, inode_table_bit_map, inode_table_bm_num_bytes);
	
	// Write blocks to disk
	write_blocks(BLOCK_INDEX_INODET, 8, buffer);
}

void write_free_bm_to_disk() {
	// Initialize buffer
	char buffer[BLOCK_SIZE];
	memset(buffer, 0, BLOCK_SIZE);
	
	// Put free_bit_map into buffer
	unsigned int free_bm_num_bytes = FREE_BM_SIZE*sizeof(uint8_t);
	memcpy(buffer+0, free_bit_map, free_bm_num_bytes);

	// Write free_bit_map to disk
	write_blocks(BLOCK_INDEX_FREE_BITMAP, 1, buffer);
}

void read_superblock_from_disk() {
	// Initialize buffer
	char buffer[BLOCK_SIZE];
	memset(buffer, 0, BLOCK_SIZE);
	
	// Read superblock into buffer
	read_blocks(BLOCK_INDEX_SUPERBLOCK, 1, buffer);
	
	// Copy to super_block struct
	memcpy(&super_block, buffer+0, sizeof(superblock_t));
}

void read_inodet_from_disk() {
	// Initialize buffer
	char buffer[8*BLOCK_SIZE];
	memset(buffer, 0, 8*BLOCK_SIZE);
	
	// Read individual blocks from disk to buffer
	read_blocks(BLOCK_INDEX_INODET, 8, buffer);
	
	// Copy buffer content to inode_table and inode_table_bit_map
	unsigned int inode_table_num_bytes = NUM_INODES*sizeof(inode_t);
	unsigned int inode_table_bm_num_bytes = INODE_TABLE_BM_SIZE*sizeof(uint8_t);
	memcpy(&inode_table, buffer+0, inode_table_num_bytes);
	memcpy(&inode_table_bit_map, buffer+inode_table_num_bytes, inode_table_bm_num_bytes);
}

void read_rootDir_from_disk() {
	// Initialize buffer
	char buffer[3*BLOCK_SIZE];
	memset(buffer, 0, 3*BLOCK_SIZE);
	
	// Read individual blocks from disk to buffer
	read_blocks(inode_table[inodeIndexForRootDir].data_ptrs[0], 1, buffer+0);
	read_blocks(inode_table[inodeIndexForRootDir].data_ptrs[1], 1, buffer+BLOCK_SIZE);
	read_blocks(inode_table[inodeIndexForRootDir].data_ptrs[2], 1, buffer+2*BLOCK_SIZE);
	
	// Copy buffer content to rootDir and dir_entries_bit_map
	unsigned int rootDir_num_bytes = NUM_INODES*sizeof(directory_entry);
	unsigned int dir_entries_bm_num_bytes = DIR_ENTRIES_BM_SIZE*sizeof(uint8_t);
	memcpy(&rootDir, buffer+0, rootDir_num_bytes);
	memcpy(&dir_entries_bit_map, buffer+rootDir_num_bytes, dir_entries_bm_num_bytes);
}

void read_free_bm_from_disk() {
	// Initialize buffer
	char buffer[BLOCK_SIZE];
	memset(buffer, 0, BLOCK_SIZE);
	
	// Read data block bitmaps from disk to buffer
	read_blocks(BLOCK_INDEX_FREE_BITMAP, 1, buffer);
	
	// Copy buffer content to free_bit_map
	unsigned int free_bm_num_bytes = FREE_BM_SIZE*sizeof(uint8_t);
	memcpy(&free_bit_map, buffer+0, free_bm_num_bytes);
}

void open_rootDir_in_fdt() {
	int rootDirIndexForFdtable = get_index(fd_table_bit_map);
	fd_table[rootDirIndexForFdtable].inode = &inode_table[inodeIndexForRootDir];
	fd_table[rootDirIndexForFdtable].inodeIndex = dirEntryIndexForRootDir;
	fd_table[rootDirIndexForFdtable].rwptr = 0;
}

void mksfs(int fresh) {
	init_fdt();
	if(fresh==1) {
		init_fresh_disk(LASTNAME_FIRSTNAME_DISK, BLOCK_SIZE, NUM_TOTAL_BLOCKS);
		init_free_bm();
		init_inodet();
		init_super();
		write_superblock_to_disk();
		init_rootDir();
		
		write_rootDir_to_disk();
		write_inodet_to_disk();
		write_free_bm_to_disk();
		open_rootDir_in_fdt();
	}
	else {
		init_disk(LASTNAME_FIRSTNAME_DISK, BLOCK_SIZE, NUM_TOTAL_BLOCKS);
		read_superblock_from_disk();
		read_inodet_from_disk();
		read_rootDir_from_disk();
		read_free_bm_from_disk();
		open_rootDir_in_fdt();
	}
}

int sfs_getnextfilename(char *fname){
	// Check if pointer is at null file
	if(dirEntryTrackerIndex < 0 || dirEntryTrackerIndex >= NUM_INODES) {
		dirEntryTrackerIndex = 0;
		return 0;
	}

	// Find next valid file
	while (dirEntryTrackerIndex < NUM_INODES) {
		if (rootDir[dirEntryTrackerIndex].name[0] != '\0') {
			strcpy(fname, rootDir[dirEntryTrackerIndex].name);
			dirEntryTrackerIndex++;
			return 1;
		} else {
			dirEntryTrackerIndex++;
		}
	}
	dirEntryTrackerIndex = 0;
	return 0;
}

int sfs_getfilesize(const char* path){
	for(int i=0; i<NUM_INODES; i++) {
		// Found file path in dir entry
		if(strcmp(rootDir[i].name, path)==0) {
			//Check if inode is valid
			if(rootDir[i].num < 0) {
				#ifdef PRINT_ERRORS
				printf("! sfs_getfilesize: found %s at rootDir[%d] but inode %d invalid\n", path, i, rootDir[i].num);
				#endif
				return -1;
			}
			//printf("- sfs_getfilesize(%s): returning inode_table[rootDir[%d].num=%d].size=%d\n", path, i, rootDir[i].num, inode_table[rootDir[i].num].size);
			return inode_table[rootDir[i].num].size;
		}
	}
	
	// No file path found in dir entry
	#ifdef PRINT_ERRORS
	printf("! sfs_getfilesize: did not find %s in rootDir\n", path);
	debug_print_root_dir_entries();
	#endif
	return -1;
}

int check_filenamevalidity(char *name) {
	if(name==NULL) {
		return 0;
	}
	int n = strlen(name);
	// Filename with extension has more than 20 chars
	if(n>20) {
		return 0;
	}
	
	int periodIndex = -1;
	for(int i=0; i<n; i++) {
		if(name[i]=='.') {
			periodIndex = i;
		}
	}
	
	// No extension so check if file name has max 16 chars
	if(periodIndex == -1) {
		if(n>16) {
			return 0;
		}
		else {
			return 1;
		}
	}
	
	// Extension exists so check if file name has max 16 chars and ext has max 3 chars
	int filenamelen = periodIndex;
	if(filenamelen>16) {
		return 0;
	}
	int extlen = n-(periodIndex+1);
	if(extlen>3) {
		return 0;
	}
	return 1;
}

void debug_print_inode_table_entries() {
	printf("--- debug_print_inode_table_entries ---\n");
	for (int i = 0; i < NUM_INODES; ++i) {
		if (inode_table[i].size < 0) {
			//printf("%d: EMPTY\n", i);
			continue;
		} else {
			printf("%d: size=%d ptrs 0 1 11 ind: %d %d %d %d\n", i, inode_table[i].size, inode_table[i].data_ptrs[0], inode_table[i].data_ptrs[1], inode_table[i].data_ptrs[11], inode_table[i].indirectPointer);
		}
	}
	printf("\n");
}

void debug_print_root_dir_entries() {
	printf("--- debug_print_root_dir_entries ---\n");
	for(int i=0; i<NUM_INODES; i++) {
		if (rootDir[i].name[0] == '\0') {
			//printf("rootDir[%d]: EMPTY\n", i);
			continue;
		} else {
			printf("rootDir[%d]: %s, %d\n", i, rootDir[i].name, rootDir[i].num);
			int inodeIndex = rootDir[i].num;
			printf("- size: %d\n", inode_table[inodeIndex].size);
			printf("- ptrs 0 1 11 ind: %d %d %d %d\n", (int) inode_table[inodeIndex].data_ptrs[0], (int) inode_table[inodeIndex].data_ptrs[1], (int) inode_table[inodeIndex].data_ptrs[11], (int) inode_table[inodeIndex].indirectPointer);
		}
	}
	printf("\n");
}

int sfs_fopen(char *name){
	#ifdef PRINT_FN_CALLS
	printf("- sfs_fopen(%s)\n", name);
	#endif

	// Validate format for name
	int isValid=check_filenamevalidity(name);
	if(!isValid) {
		#ifdef PRINT_ERRORS
		printf("- sfs_fopen: filename %s invalid\n", name);
		#endif
		return -1;
	}

	int inodeNum = -1;
	// Check if file exists in rootDir
	for(int i=0; i<NUM_INODES; i++) {
		if(strcmp(rootDir[i].name,name)==0) {
			inodeNum = rootDir[i].num;
			break;
		}
	}
	
	// File exists
	if(inodeNum != -1){
		// Check if file exists in file descriptor
		for(int i=0; i<NUM_INODES; i++) {
			// File already open, set pointer to append mode
			if(fd_table[i].inodeIndex==inodeNum){
				fd_table[i].rwptr = inode_table[inodeNum].size;
				#ifdef PRINT_ERRORS
				printf("! sfs_fopen: returning opened fileID %d for existing %s\n", i, name);
				#endif
				return i;
			}
		}

		// File not open so open in file descriptor and set pointer to append mode
		int fdtIndex = get_index(fd_table_bit_map);
		fd_table[fdtIndex].rwptr = inode_table[inodeNum].size;
		fd_table[fdtIndex].inode = &inode_table[inodeNum];
		fd_table[fdtIndex].inodeIndex = inodeNum;
		#ifdef PRINT_SFS_FOPEN
		printf("- sfs_fopen: returning new fd id %d for existing %s\n", fdtIndex, name);
		#endif
		return fdtIndex;
	}
	
	// File does not exist so create inode, add to dir entries, and open in file descriptor
	else {
		int inodeTableIndex = get_index(inode_table_bit_map);
		if (inodeTableIndex < 0 || inodeTableIndex >= NUM_INODES) { // cannot have more than NUM_INODES files total in sfs
			#ifdef PRINT_ERRORS
			printf("! sfs_fopen: refusing to create %s since get_index(inode)=%d\n", name, inodeTableIndex);
			#endif
			return -1;
		}
		inode_table[inodeTableIndex].size = 0;
		inode_table[inodeTableIndex].indirectPointer = -1;
		for(int j=0; j<12; j++) {
			inode_table[inodeTableIndex].data_ptrs[j] = -1;
		}
		
		int dirEntryIndex = get_index(dir_entries_bit_map);
		rootDir[dirEntryIndex].num = inodeTableIndex;
		strcpy(rootDir[dirEntryIndex].name, name);
		rootDir[dirEntryIndex].name[MAX_FILE_NAME-1] = '\0';
		
		int fdtIndex = get_index(fd_table_bit_map);
		fd_table[fdtIndex].rwptr = inode_table[inodeTableIndex].size;
		fd_table[fdtIndex].inode = &inode_table[inodeTableIndex];
		fd_table[fdtIndex].inodeIndex = inodeTableIndex;
		
		write_inodet_to_disk();
		write_rootDir_to_disk();
		
		#ifdef PRINT_SFS_FOPEN
		printf("- sfs_fopen: returned fd id %d for newly created %s, with inode_table[%d].inodeIndex=%d\n", fdtIndex, name, fdtIndex, inodeTableIndex);
		#endif
		return fdtIndex;	
	}
}

int sfs_fclose(int fileID) {
	#ifdef PRINT_FN_CALLS
	printf("- sfs_fclose(%d)\n", fileID);
	#endif
	
	if(fileID<0 || fileID>=NUM_INODES) {
		return -1;
	}
	if (fd_table[fileID].inodeIndex == -1) {
		return -1;
	}
	rm_index(fd_table_bit_map, fileID);
	fd_table[fileID].rwptr = 0;
	fd_table[fileID].inode = NULL;
	fd_table[fileID].inodeIndex = -1;
	return 0;
}

int sfs_fread(int fileID, char *buf, int length) {
	#ifdef PRINT_FN_CALLS
	printf("- sfs_fread(%d, buf, %d)\n", fileID, length);
	#endif

	// validate inputs
	if(fileID < 0 || fileID>=NUM_INODES || length < 0) {
		#ifdef PRINT_ERRORS
		printf("! sfs_fread INVALID INPUTS\n");
		#endif
		return 0;
	}
	
	int inodeIndex = fd_table[fileID].inodeIndex;
	if (inodeIndex <0) {
		#ifdef PRINT_ERRORS
		printf("- sfs_fread: trying to read from non-open fd entry %d\n", fileID);
		#endif
		return 0;
	}
	
	// Check if reading more than file size
	if(fd_table[fileID].rwptr+length > inode_table[inodeIndex].size){
		length = inode_table[inodeIndex].size - fd_table[fileID].rwptr;
	}
	
	char tempBlock[BLOCK_SIZE];
	unsigned int indirPtrList[BLOCK_SIZE/sizeof(unsigned int)];
	// If I need to access indirect Pointer data, read from disk to indirptrlist 
	if(CEILING(fd_table[fileID].rwptr+length, BLOCK_SIZE) > 12){
		read_blocks(inode_table[inodeIndex].indirectPointer, 1, (char*) indirPtrList);
		#ifdef PRINT_SFS_FREAD
		printf("- sfs_fread: reading indPtrList from inode.indirectPointer %d\n", inode_table[inodeIndex].indirectPointer);
		#endif
	}
	
	int num_bytes_read = 0;
	while(num_bytes_read < length) {
	  // compute current block based on rwptr
	  int dataBlockIndex = FLOOR(fd_table[fileID].rwptr, BLOCK_SIZE);
	  #ifdef PRINT_SFS_FREAD
	  printf("- sfs_fread: dataBlockIndex: %d, fd_table[fileID].rwptr: %ld\n", dataBlockIndex, fd_table[fileID].rwptr);
	  #endif
	  
	  // compute byteOffset based on current block and rwptr
	  int byteOffset = fd_table[fileID].rwptr - dataBlockIndex*BLOCK_SIZE;
	  
	  // If accessing indirect pointer data, load a block to tempblock 
	  if(dataBlockIndex>=12) {
		read_blocks(indirPtrList[dataBlockIndex-12], 1, tempBlock);
		#ifdef PRINT_SFS_FREAD
		printf("- sfs_fread: loading from indPtrList[%d], block=%d, first/last entry: %d %d\n", dataBlockIndex-12, indirPtrList[dataBlockIndex-12], tempBlock[0], tempBlock[255]);
		#endif
	  }
	  // If accessing direct pointer data, load a block to tempblock
	  else {
		read_blocks(inode_table[inodeIndex].data_ptrs[dataBlockIndex], 1, tempBlock);
		#ifdef PRINT_SFS_FREAD
		printf("- sfs_fread: loading from data_ptrs[%d], block=%d, first/last entry: %d %d\n", dataBlockIndex, inode_table[inodeIndex].data_ptrs[dataBlockIndex], tempBlock[0], tempBlock[255]);
		#endif
	  }
	  
	  int num_bytes_to_read = length-num_bytes_read;
	  if (num_bytes_to_read > BLOCK_SIZE - byteOffset) {
		  num_bytes_to_read = BLOCK_SIZE - byteOffset;
	  }
	  #ifdef PRINT_SFS_FREAD
	  printf("- sfs_fread: num_bytes_to_read: %d\n", num_bytes_to_read);
	  #endif
      memcpy(buf+num_bytes_read, tempBlock+byteOffset, num_bytes_to_read);
	  num_bytes_read += num_bytes_to_read;
	  fd_table[fileID].rwptr += num_bytes_to_read;
	  #ifdef PRINT_SFS_FREAD
	  printf("- sfs_fread: num_bytes_read: %d, fd_table[fileID].rwptr: %ld\n", num_bytes_read, fd_table[fileID].rwptr);
	  #endif
	}
	
	return length;
 
}

int sfs_fwrite(int fileID, const char *buf, int length) {
	#ifdef PRINT_FN_CALLS
	printf("- sfs_fwrite(%d, buf, %d)\n", fileID, length);
	#endif
	
	// Validate inputs
	if(fileID < 0 || fileID>=NUM_INODES || length < 0) {
		return 0;
	}
	
	unsigned int indirPtrList[BLOCK_SIZE/sizeof(unsigned int)];
	for (int i = 0; i < BLOCK_SIZE/sizeof(unsigned int); ++i) indirPtrList[i] = -1;
	
	// If more space is needed, pre-allocate blocks
	int inodeIndex = fd_table[fileID].inodeIndex;
	int file_size = inode_table[inodeIndex].size;
	int numFullBlocks = FLOOR(file_size, BLOCK_SIZE);
	int numBytesInLastBlock = file_size - BLOCK_SIZE*numFullBlocks;
	int numFreeBytesInLastBlock = BLOCK_SIZE - numBytesInLastBlock;
	int numBytesToAppend = fd_table[fileID].rwptr+length-file_size;
	int numBytesToAppendNeedingNewBlocks = numBytesToAppend - numFreeBytesInLastBlock;
	int numBlocksNeeded = CEILING(numBytesToAppendNeedingNewBlocks, BLOCK_SIZE);
	
	int totalBlocks = CEILING((file_size+numBytesToAppend), BLOCK_SIZE);

	// If we need more than 12 direct pointers, check if indirect pointer is initialized
	// If indirect pointer is not initialized then we create indirect pointer list (size = BLOCK_SIZE)
	if(totalBlocks > 12 && inode_table[inodeIndex].indirectPointer==-1) {
		int new_index = get_index(free_bit_map);
		if (new_index >= NUM_TOTAL_BLOCKS || new_index < 0) {
			#ifdef PRINT_ERRORS
			printf("! sfs_fwrite: refusing to write more because out of free blocks needed for indirPtrList\n");
			#endif
			return 0;
		}
		inode_table[inodeIndex].indirectPointer = new_index;
		#ifdef PRINT_SFS_FWRITE
		printf("- sfs_fwrite: allocating block %d for inode[%d].indirPtrList\n", inode_table[inodeIndex].indirectPointer, inodeIndex);
		#endif
		for(int i=0; i<(BLOCK_SIZE/sizeof(unsigned int)); i++) {
			indirPtrList[i] = -1;
		}
	} 
	else if (totalBlocks > 12 && inode_table[inodeIndex].indirectPointer != -1) {
		// Read from block and into indirPointer		
		read_blocks(inode_table[inodeIndex].indirectPointer, 1, (char*) indirPtrList);
		#ifdef PRINT_SFS_FWRITE
		printf("- fwrite: read block %d for inode[%d].indirPtrList into memory (%d %d %d %d)\n", inode_table[inodeIndex].indirectPointer, inodeIndex, indirPtrList[0], indirPtrList[1], indirPtrList[254], indirPtrList[255]);
		#endif
	}
	
	// For each block allocate data block bm index into inode
	if(numBlocksNeeded > 0) {
		int numBlockExisting = CEILING(file_size, BLOCK_SIZE);
		#ifdef PRINT_SFS_FWRITE
		printf("- existing file_size: %d\n", file_size);
		printf("- numBlocksNeeded: %d\n", numBlocksNeeded);
		printf("- numBlockExisting: %d\n", numBlockExisting);
		printf("- totalBlocks: %d\n", totalBlocks);
		#endif
		if(totalBlocks <=12) {
			for(int i=numBlockExisting; i<totalBlocks; i++) {
				int new_index = get_index(free_bit_map);
				if (new_index < 0 || new_index >= NUM_TOTAL_BLOCKS) {
					#ifdef PRINT_ERRORS
					printf("! sfs_fwrite: refusing to write more because out of free blocks needed for inode[%d].data_ptrs[%d]\n", inodeIndex, i);
					#endif
					return 0;
				}
				inode_table[inodeIndex].data_ptrs[i] = new_index;
				#ifdef PRINT_SFS_FWRITE
				printf("- fwrite: allocating block %d for inode[%d].data_ptrs[%d]\n", inode_table[inodeIndex].data_ptrs[i], inodeIndex, i);
				#endif
			}
		}
		else{
			for(int i=numBlockExisting; i<totalBlocks; i++) {
				if (i < 12) {
					int new_index = get_index(free_bit_map);
					if (new_index < 0 || new_index >= NUM_TOTAL_BLOCKS) {
						#ifdef PRINT_ERRORS
						printf("- sfs_fwrite: refusing to write more because out of free blocks needed for inode[%d].data_ptrs[%d]\n", inodeIndex, i);
						#endif
						return 0;
					}
					inode_table[inodeIndex].data_ptrs[i] = new_index;
					#ifdef PRINT_SFS_FWRITE
					printf("- fwrite: allocating block %d for inode[%d].data_ptrs[%d]\n", inode_table[inodeIndex].data_ptrs[i], inodeIndex, i);
					#endif
				}
				else {
					int new_index = get_index(free_bit_map);
					if (new_index < 0 || new_index >= NUM_TOTAL_BLOCKS) {
						#ifdef PRINT_ERRORS
						printf("- sfs_fwrite: refusing to write more because out of free blocks needed for inode[%d].indirPtrList[%d]\n", inodeIndex, i-12);
						#endif
						return 0;
					}
					indirPtrList[i-12] = new_index;
					#ifdef PRINT_SFS_FWRITE
					printf("- fwrite: allocating block %d for inode[%d].indirPtrList[%d]\n", indirPtrList[i-12], inodeIndex, i-12);
					#endif
				}
			}
		}
	}

	char tempBlock[BLOCK_SIZE];
	memset(tempBlock, 0, BLOCK_SIZE);
	
	// while there are more blocks of content to be written:
	unsigned int num_bytes_written = 0;
	while(num_bytes_written < length) {
		// compute the data block index corresponding to rwptr
		int dataBlockIndex = FLOOR(fd_table[fileID].rwptr, BLOCK_SIZE);
		
		// load that data block into local memory from the disk
		if(dataBlockIndex>=12) {
			read_blocks(indirPtrList[dataBlockIndex-12], 1, tempBlock);
			#ifdef PRINT_SFS_FWRITE
			printf("- sfs_fwrite: while(%d < %d) read ind_ptr[%d] (=%d) into tempBlock (%d %d | %d %d)\n", num_bytes_written, length, dataBlockIndex-12, indirPtrList[dataBlockIndex-12], tempBlock[0], tempBlock[1], tempBlock[254], tempBlock[255]);
			#endif
		}
		else {
			read_blocks(inode_table[inodeIndex].data_ptrs[dataBlockIndex], 1, tempBlock);
			#ifdef PRINT_SFS_FWRITE
			printf("- sfs_fwrite: while(%d < %d) read data_ptr[%d] (=%d) into tempBlock (%d %d | %d %d)\n", num_bytes_written, length, dataBlockIndex, inode_table[inodeIndex].data_ptrs[dataBlockIndex], tempBlock[0], tempBlock[1], tempBlock[254], tempBlock[255]);
			#endif
		}
		
		// compute byte-offset within this block based on rwptr
		int byteOffset = fd_table[fileID].rwptr - dataBlockIndex*BLOCK_SIZE;
		
		// starting from byte-offset, write N bytes from buf to data block, where N = min(1024-(byte-offset), length-of-buf-left-to-be-written)
		int num_bytes_to_write = length-num_bytes_written;
		if (num_bytes_to_write > BLOCK_SIZE-byteOffset) {
			num_bytes_to_write = BLOCK_SIZE-byteOffset;
		}
		memcpy(tempBlock+byteOffset, buf+num_bytes_written, num_bytes_to_write);
		#ifdef PRINT_SFS_FWRITE
		printf("- sfs_fwrite: ... memcpy buf+%d -> tempBlock+%d for %d bytes\n", num_bytes_written, byteOffset, num_bytes_to_write);
		#endif
		num_bytes_written += num_bytes_to_write;
		fd_table[fileID].rwptr += num_bytes_to_write;
		
		// write the local data block back into disk
		if(dataBlockIndex>=12) {
			if (indirPtrList[dataBlockIndex-12] < 0 || indirPtrList[dataBlockIndex-12] >= NUM_TOTAL_BLOCKS) {
				#ifdef PRINT_ERRORS
				printf("! sfs_fwrite: !!!!!ERROR!!!!! trying to write_blocks(indirPtrList[%d] with invalid start address: %d\n", dataBlockIndex-12, indirPtrList[dataBlockIndex-12]);
				#endif
				return 0; // IDEALLY: fix this bug instead of force-returning 0 half-way
			}
			
			
			write_blocks(indirPtrList[dataBlockIndex-12], 1, tempBlock);
			#ifdef PRINT_SFS_FWRITE
			printf("- sfs_fwrite: ... wrote tempBlock (%d %d | %d %d) back to ind_ptr[%d] (=%d)\n", tempBlock[0], tempBlock[1], tempBlock[254], tempBlock[255], dataBlockIndex-12, indirPtrList[dataBlockIndex-12]);
			#endif
		}
		else {
			if (inode_table[inodeIndex].data_ptrs[dataBlockIndex] < 0 || inode_table[inodeIndex].data_ptrs[dataBlockIndex] >= NUM_TOTAL_BLOCKS) {
				#ifdef PRINT_ERRORS
				printf("! sfs_fwrite: !!!!!ERROR!!!!! trying to write_blocks(inode_table[%d].data_ptrs[%d]) with invalid start address: %d\n", inodeIndex, dataBlockIndex, inode_table[inodeIndex].data_ptrs[dataBlockIndex]);
				#endif
				return 0; // IDEALLY: fix this bug instead of force-returning 0 half-way
			}

			write_blocks(inode_table[inodeIndex].data_ptrs[dataBlockIndex], 1, tempBlock);
			#ifdef PRINT_SFS_FWRITE
			printf("- sfs_fwrite: ... wrote tempBlock (%d %d | %d %d) back to data_ptr[%d] (=%d)\n", tempBlock[0], tempBlock[1], tempBlock[254], tempBlock[255], dataBlockIndex, inode_table[inodeIndex].data_ptrs[dataBlockIndex]);
			#endif
		}
	}	  
	
	// Update the file size in the inode table entry
	inode_table[inodeIndex].size += numBytesToAppend;
	
	if(inode_table[inodeIndex].indirectPointer != -1) {
		// Write back indirPtrList into data block
		write_blocks(inode_table[inodeIndex].indirectPointer, 1, (char*) indirPtrList);
		#ifdef PRINT_SFS_FWRITE
		printf("- fwrite: wrote back indirPtrList to block %d (list entries: %d %d %d %d)\n", inode_table[inodeIndex].indirectPointer, indirPtrList[0], indirPtrList[1], indirPtrList[254], indirPtrList[255]);
		#endif
	}
	
	write_inodet_to_disk();
	write_free_bm_to_disk();
	
	return length;
}

int sfs_fseek(int fileID, int loc) {
	#ifdef PRINT_FN_CALLS
	printf("- sfs_fseek(%d, %d)\n", fileID, loc);
	#endif
	
	if(fileID<0 || fileID>=NUM_INODES) {
		return -1;
	}
	int inodeIndex = fd_table[fileID].inodeIndex;
	if(loc > inode_table[inodeIndex].size || loc<0) {
		return -1;
	}
	fd_table[fileID].rwptr = loc;
	return 0;
}

int sfs_remove(char *file) {
	#ifdef PRINT_FN_CALLS
	printf("- sfs_fremove(%s)\n", file);
	#endif
	
	int fileExists = 0;
	int inodeIndex;
	for(int i=0; i<NUM_INODES; i++) {
		if(strcmp(rootDir[i].name, file) == 0) {
			fileExists=1;
			inodeIndex=rootDir[i].num;
			rootDir[i].num = -1;
			for(int j=0; j<MAX_FILE_NAME; j++) {
				rootDir[i].name[j]= '\0';
			}
			break;
		}
	}
	if(!fileExists) {
		#ifdef PRINT_ERRORS
		printf("! sfs_fremove failed: file does not exist\n");
		#endif
		return -1;
	}
  
	for(int i=0; i<NUM_INODES; i++) {
		if(fd_table[i].inodeIndex == inodeIndex) {
			sfs_fclose(i);
			break;
		}
	}
	unsigned int indirPtrList[BLOCK_SIZE/sizeof(unsigned int)];
	if(inode_table[inodeIndex].indirectPointer != -1) {
		read_blocks(inode_table[inodeIndex].indirectPointer, 1, (char*) indirPtrList);
		for(int i=0; i<BLOCK_SIZE/sizeof(unsigned int); i++) {
			if(indirPtrList[i] != -1) {
				rm_index(free_bit_map, indirPtrList[i]);
			}
		}
		rm_index(free_bit_map, inode_table[inodeIndex].indirectPointer);
		inode_table[inodeIndex].indirectPointer = -1;
	}
	
	for(int i=0; i<12; i++) {
		if(inode_table[inodeIndex].data_ptrs[i] != -1) {
			rm_index(free_bit_map, inode_table[inodeIndex].data_ptrs[i]);
			inode_table[inodeIndex].data_ptrs[i] = -1;
		}
	}
	inode_table[inodeIndex].size = -1;
	
	rm_index(inode_table_bit_map, inodeIndex);

	// Write data blocks bitmap back to disk since I freed a bunch of data blocks
	write_free_bm_to_disk();
	// Write inode table back to disk (since I removed the inode)
	write_inodet_to_disk();
	// Write rootDir back to disk (since I modified dir_entries)
	write_rootDir_to_disk();
	
	return 0;
}







