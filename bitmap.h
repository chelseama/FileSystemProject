#ifndef _INCLUDE_BITMAP_H_
#define _INCLUDE_BITMAP_H_

#include <stdint.h>

/* macros */
#define FREE_BIT(_data, _which_bit) \
    _data = _data | (1 << _which_bit)

#define USE_BIT(_data, _which_bit) \
    _data = _data & ~(1 << _which_bit)
    

/*
 * @short force an index to be set.
 * @long Use this to setup your superblock, inode table and free bit map
 *       This has been left unimplemented. You should fill it out.
 *
 * @param index index to set 
 *
 */
void force_set_index(uint8_t* free_bit_map, uint32_t index);

/*
 * @short find the first free data block
 * @return index of data block to use
 */
uint32_t get_index(uint8_t* free_bit_map);

/*
 * @short frees an index
 * @param index the index to free
 */
void rm_index(uint8_t* free_bit_map, uint32_t index);

#endif //_INCLUDE_BITMAP_H_


