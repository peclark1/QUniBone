/* filesystem_xxdp.cpp - XXDP file system


  Copyright (c) 2022, Joerg Hoppe
  j_hoppe@t-online.de, www.retrocmp.com

  All rights reserved.

  Redistribution and use in source and binary forms, with or without
  modification, are permitted provided that the following conditions
  are met:

  - Redistributions of source code must retain the above copyright
    notice, this list of conditions and the following disclaimer.

  - Redistributions in binary form must reproduce the above copyright
    notice, this list of conditions and the following disclaimer in the
    documentation and/or other materials provided with the distribution.

  - Neither the name of the copyright holder nor the names of its
    contributors may be used to endorse or promote products derived from
    this software without specific prior written permission.

  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
  A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
  HOLDERS OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
  SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED
  TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
  PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
  LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
  NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
  SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

  06-jan-2022 JH      created from tu58fs

 The logical structure of the DOS-11 file system
 is represented by linked data structures.
 The logical filesystem is indepented of the physical image,
 file content and blocklist are hold in own buffers.

 API
  _init():
 Clear all data from the filesystem and preload
 layout parameters (length of preallocated areas, interleave etc.)
 from device-specific "Random Access Device Information" table.

 _parse():
  build the logical filesystem from a physical binary image.
  After that, user files and boot files can be read.

  _add_boot(), _add_file():
  add boot files and user files to the logical image,
  (allocate blocks and update blocklists)

  _render()
  Produce the binary image from logical filesystem.


 */

#include <string.h>
#include <inttypes.h> // PRI* formats

#include "logger.hpp"

#include "filesystem_xxdp.hpp"


// pseudo file for volume parameters
#define XXDP_VOLUMEINFO_BASENAME  "$VOLUM" // valid XXDP file name
#define XXDP_VOLUMEINFO_EXT 	  "INF"
// boot block and monitor blocks are pseudo files
#define XXDP_BOOTBLOCK_BASENAME   "$BOOT"  // valid XXDP file names
#define XXDP_BOOTBLOCK_EXT		  "BLK"
#define XXDP_MONITOR_BASENAME 	  "$MONI" // valid XXDP file names
#define XXDP_MONITOR_EXT		  "TOR"





namespace sharedfilesystem {


//xxdp_blocknr_t xxdp_linked_block_list_c::next_block_nr(xxdp_blocknr_t block_nr)
//{
//}

xxdp_linked_block_c::xxdp_linked_block_c(filesystem_xxdp_c *_filesystem): block_cache_dec_c(_filesystem)
{
    set_size(filesystem->get_block_size()) ;
}


// search a block with given number in the linked list. Null if not there
xxdp_linked_block_c* xxdp_linked_block_list_c::get_block_by_nr(xxdp_blocknr_t _block_nr)
{
    xxdp_linked_block_c *block = nullptr;

    // find cache block in list
    for(auto it = begin(); it != end(); ++it) {
        block = &(*it) ;
        if (block->get_block_nr() == _block_nr)
            break ; // found
    }
    if (block->get_block_nr() != _block_nr)
        return nullptr ;
    else
        return block ;
}




/*
// here indexed by block relative wordnr!, not byte offset
uint16_t xxdp_linked_block_list_c::block_get_word(xxdp_blocknr_t block_nr, uint8_t word_offset)
{
		xxdp_linked_block_c *block = get_block_by_nr(block_nr) ;
	assert(block != nullptr) ;	// must be found in linked list.

	return block->get_word_at_byte_offset(2*word_offset) ; // LSB first
}

void xxdp_linked_block_list_c::block_set_word(xxdp_blocknr_t block_nr,       uint8_t word_offset, uint16_t val)
{
	xxdp_linked_block_c *block = get_block_by_nr(block_nr) ;
	assert(block != nullptr) ;	// must be found in linked list.

	block->set_word_at_byte_offset(2*word_offset, val) ; // LSB first
}
*/

/*
xxdp_blocknr_t xxdp_linked_block_list_c::get_next_block_nr(xxdp_blocknr_t block_nr)
{
    xxdp_linked_block_c *block = get_block_by_nr(block_nr) ;
    assert(block != nullptr) ;
    return block->get_word_at_byte_offset(0) ;
}
*/

// add an block cache, link to previous list end, and init with 00s
void xxdp_linked_block_list_c::add_empty_block(xxdp_blocknr_t block_nr)
{
    xxdp_linked_block_c new_tail_block(this->filesystem) ;
    new_tail_block.init_zero(block_nr, 1) ;
    if (size() > 0) {
        xxdp_linked_block_c &prev_tail_block = at(size()-1) ;
        // point first word of previous list tail to new new_tail_block nr
        prev_tail_block.set_word_at_byte_offset(0, block_nr) ;
    }
    // terminate new list end
    new_tail_block.set_word_at_byte_offset(0, 0) ;
    push_back(new_tail_block) ;
}

void xxdp_linked_block_list_c::add_empty_blocks(xxdp_block_nr_list_c *block_nr_list)
{
    for(unsigned i=0 ; i < block_nr_list->size() ; i++)
        add_empty_block(block_nr_list->at(i)) ;
}


// scan linked list at block # 'start'
// bytes 0,1 = 1st word in block are # of next. Last blck has link "0".
void xxdp_linked_block_list_c::load_from_image(xxdp_blocknr_t start_block_nr)
{
    xxdp_linked_block_c block(this->filesystem) ;
    xxdp_blocknr_t block_nr = start_block_nr;
    clear() ;
    do {
        block.load_from_image(block_nr, 1) ;
        push_back(block) ;
        // follow link to next block
        block_nr = block.get_word_at_byte_offset(0) ;
    } while (size() < XXDP_MAX_BLOCKS_PER_LIST && block_nr > 0);
    if (block_nr > 0)
        throw filesystem_exception("xxdp_linked_block_list_c::load_from_image(): block list too long or recursion");
}


// valid struct:
// a linked list consists of 1-block caches in order of the link word
// in preceding block
void xxdp_linked_block_list_c::verify()
{
    xxdp_blocknr_t prev_link_block_nr ; // link to current block fro moredecessor
    prev_link_block_nr = 0 ; // invalid on 1st block
    for(auto it = begin(); it != end(); ++it) {
        xxdp_linked_block_c *block = &(*it) ;
        assert(block->size() == filesystem->get_block_size()) ;
        if (it != begin()) {
            // correctly pointed to?
            assert(prev_link_block_nr > 0) ;
            assert(prev_link_block_nr == block->get_block_nr()) ;
        }
        prev_link_block_nr = block->get_word_at_word_offset(0) ;
    }
    // last block must have  0 link
    assert(prev_link_block_nr == 0) ;
}


// write linked block list to image
void xxdp_linked_block_list_c::write_to_image()
{
    for(auto it = begin(); it != end(); ++it) {
        xxdp_linked_block_c *block = &(*it) ;
        block->flush_to_image() ;
    }
}



// make indexed list of all blocks
void xxdp_linked_block_list_c::get_block_nr_list(xxdp_block_nr_list_c *block_nr_list)
{
    block_nr_list->clear() ;
    for (auto it=begin() ; it != end() ; ++it)
        block_nr_list->push_back(it->get_block_nr()) ;
}


// stream the 510 payload bytes of each block into a dec_stream's byte buffer
void xxdp_linked_block_list_c::write_to_file_buffer(file_xxdp_c *f)
{
    unsigned block_datasize = filesystem->get_block_size() - 2; // data amount in block, after link word
    unsigned byte_count = size() * block_datasize;
    f->set_size(byte_count) ; // data stream
    f->file_size = byte_count ;
    uint8_t	*dst = f->data_ptr() ;
    for (auto it = begin() ; it != end() ; ++it) {
        // cat data from all blocks
        xxdp_linked_block_c *block = &(*it) ;
        uint8_t *src = block->data_ptr() + 2 ; // skip link word
        memcpy(dst, src, block_datasize);
        dst += block_datasize;
        assert(dst <= f->data_ptr() + f->file_size);
    }
}

// load the 510 payload bytes of each block from a dec_stream's byte buffer
// block_list must have been pre-allocated with add_empty_blocks()
void xxdp_linked_block_list_c::load_from_file_buffer(file_xxdp_c *f)
{
    unsigned block_datasize = filesystem->get_block_size() - 2; // data amount in block, after link word

    unsigned bytes_to_copy = f->file_size;
    uint8_t *src = f->data_ptr();
    for (auto it = begin() ; it != end() ; ++it) {
        // cat data to all blocks
        xxdp_linked_block_c *block = &(*it) ;
        uint8_t *dst = block->data_ptr() + 2 ; // skip link word
        if (bytes_to_copy < block_datasize) // EOF?
            block_datasize = bytes_to_copy;
        memcpy(dst, src, block_datasize);
        src += block_datasize;
        bytes_to_copy -= block_datasize;
        assert(src <= f->data_ptr() + f->file_size);
    }
    assert(bytes_to_copy == 0);

}

void xxdp_linked_block_list_c::print_diag(std::ostream &stream, const char *info)
{
    printf("%s\n", info);
    for (unsigned i=0 ; i < size() ; i++) {
        // cat data to all blocks
        xxdp_linked_block_c *block = &at(i) ;
        printf("block[%d]: nr=%d, next=%d, data=\n", i,
               (unsigned)block->get_block_nr(), (unsigned)block->get_next_block_nr()) ;
        hexdump(stream, block->data_ptr(), block->size(), nullptr);
    }

}



// empty block bitmap
void xxdp_bitmap_c::clear()
{
    block_list.clear() ;
    for (unsigned i = 0; i < XXDP_MAX_BLOCKCOUNT; i++)
        used[i] = false;
}


// get count of used blocks, by counting bitmap flags
int xxdp_bitmap_c::used_block_count()
{
    int result = 0;
    for (unsigned i = 0; i < block_list.size(); i++) {
        xxdp_linked_block_c *map_block = &block_list[i];
        unsigned map_wordcount = map_block->get_word_at_byte_offset(2) ;
        for (unsigned j = 0; j < map_wordcount; j++) {
            uint16_t map_flags = map_block->get_word_at_byte_offset(j + 4);
            // 16 flags per word. count "1" bits
            if (map_flags == 0xffff)
                result += 16; // most of time
            else
                for (unsigned k = 0; k < 16; k++)
                    if (map_flags & (1 << k))
                        result++;
        }
    }
    return result;
}



file_xxdp_c::file_xxdp_c() :file_dec_stream_c(this, "")
{
    basename = "" ;
    ext= "" ;
}

// clone constructor. only metadata
file_xxdp_c::file_xxdp_c(file_xxdp_c *f) : file_base_c(f), file_dec_c(f), file_dec_stream_c(this,"")
{
    basename = f->basename ;
    ext = f->ext ;
//	internal = f->internal ;
}

file_xxdp_c::~file_xxdp_c() {
    // host_file_streams[] automatically destroyed
}


// NAME.EXT
string file_xxdp_c::get_filename()
{
    return filesystem_xxdp_c::make_filename(basename, ext) ;
}


string file_xxdp_c::get_host_path()
{
    // let host build the linux path, using my get_filename()
    // result is just "/filename"
    return filesystem_host_c::get_host_path(file) ;
}

// have file attributes or data content changed?
// filename not compared, speed!
// Writes to the image set the change flag
// this->has_changed(cmp) != cmp->has_changed(this)
bool file_xxdp_c::data_changed(file_base_c *_cmp)
{
    auto cmp = dynamic_cast <file_xxdp_c*>(_cmp) ;
    return changed
//			basename.compare(cmp->basename) != 0
//			|| ext.compare(cmp->ext) != 0
           ||memcmp(&modification_time, &cmp->modification_time, sizeof(modification_time)) // faster
           //	|| mktime(modification_time) == mktime(cmp->modification_time)
           || readonly != cmp->readonly
           || file_size != cmp->file_size ;
}



// dir::copy_metadata_to() not polymorph .. limits of C++ or my wisdom
// instances for each filesystem, only rt11 and xxdp, not needed for host
void directory_xxdp_c::copy_metadata_to(directory_base_c *_other_dir)
{
    auto other_dir = dynamic_cast<directory_xxdp_c *>(_other_dir) ;
    // start condition: other_dir already updated ... recursive by clone-constructor
    assert(other_dir != nullptr) ;

    // directory recurse not necessary for XXDP ... but this may serve as template
    for(unsigned i = 0; i < subdirectories.size(); ++i) {
        auto subdir = dynamic_cast<directory_xxdp_c *>(subdirectories[i]) ;
        // add to other filesystem a new copy-instance
        other_dir->filesystem->add_directory(other_dir, new directory_xxdp_c(subdir));
    }
    for(unsigned i = 0; i < files.size(); ++i) {
        auto f  = dynamic_cast<file_xxdp_c *>(files[i]) ;
        other_dir->add_file(new file_xxdp_c(f)) ;
    }
}



static int is_leapyear(int y) {
    return ((y % 4 == 0) && (y % 100 != 0)) || (y % 400 == 0);
}

static int monthlen_noleap[] = { 31, 28, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
static int monthlen_leap[] = { 31, 29, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };

// convert a DOS-11 data to time_t
// day = 5 bits, month= 4bits, year = 9bits
struct tm filesystem_xxdp_c::dos11date_decode(uint16_t w)
{
    int *monthlen;
    struct tm result;
    int y = (w / 1000) + 1970;
    int m;
    int d = w % 1000; // starts as day of year

    memset(&result, 0, sizeof(result)); //clear

    // use correct table
    monthlen = is_leapyear(y) ? monthlen_leap : monthlen_noleap;

    m = 0;
    while (d > monthlen[m]) {
        d -= monthlen[m];
        m++;
    }
    result.tm_year = y - 1900;
    result.tm_mon = m; // 0..11
    result.tm_mday = d; // 1..31

    assert(w == dos11date_encode(result)); // cross check
    return result;
}


// year after allowed: trunc to this.
int dos11date_overflow_year = 1999;
uint16_t filesystem_xxdp_c::dos11date_encode(struct tm t)
{
    int *monthlen;
    uint16_t result = 0;
    int doy;
    int y = 1900 + t.tm_year; // year is easy
    int m;

    monthlen = is_leapyear(y) ? monthlen_leap : monthlen_noleap;

    for (doy = m = 0; m < t.tm_mon; m++)
        doy += monthlen[m];

    result = doy + t.tm_mday;
    result += 1000 * (y - 1970);
    return result;
}

// join basename and ext
// with "." on empty extension "FILE."
// used as key for file map
string filesystem_xxdp_c::make_filename(string basename, string ext)
{
    string result = trim_copy(basename);
    result.append(".") ;
    result.append(trim_copy(ext)) ;
    std::transform(result.begin(), result.end(),result.begin(), ::toupper);
    return result ;
}


filesystem_xxdp_c::filesystem_xxdp_c(drive_info_c &_drive_info, unsigned _drive_unit,
                                     storageimage_base_c *_image_partition, uint64_t _image_partition_size)
    : filesystem_dec_c(_drive_info, _drive_unit, _image_partition, _image_partition_size)
{
    layout_info = get_documented_layout_info(_drive_info.drive_type) ;
    changed_blocks = new boolarray_c(layout_info.blocks_num) ;


    // create root dir.
    add_directory(nullptr, new directory_xxdp_c() ) ;
    assert(rootdir->filesystem == this) ;
//    rootdir = new directory_xxdp_c() ; // simple list of files
//    rootdir->filesystem = this ;

    // sort order for files. For regexes the . must be escaped by \.
    // and a * is .*"
    sort_group_regexes.reserve(20) ; // speed up, no relloc
    sort_add_group_pattern("XXDPSM\\.SYS") ;
    sort_add_group_pattern("XXDPXM\\.SYS") ;
    sort_add_group_pattern("DRSSM\\.SYS") ;
    sort_add_group_pattern("DRSXM\\.SYS") ; // monitor_core_image 1st on disk
    sort_add_group_pattern(".*\\.SYS") ; // the drivers
    sort_add_group_pattern("START\\..*") ; // startup script
    sort_add_group_pattern("HELP\\..*") ; // help texts
    sort_add_group_pattern(".*\\.CCC") ; // other chain files
    sort_add_group_pattern(".*\\.BIC") ;  // *.bin and *.bic
    sort_add_group_pattern(".*\\.BIN") ;  // *.bin and *.bic

    // available block = full disk capacity minus bad sector info
    assert(_image_partition_size <= _drive_info.capacity) ;
    blockcount = needed_blocks(_drive_info.get_usable_capacity());

    // if image is enlarged, the precoded layout params of the device are not sufficient
    // for the enlarged blockcount.
    //see TU58: normally 1 prealloced block for 256KB tape
    if (layout_info.blocks_num < blockcount) {
        // calculate new layout params in .layout_info
        try {
            recalc_layout_info(blockcount) ;
        } catch(filesystem_exception &e) {
            FATAL("filesystem_xxdp: Can not calculate new layout params");
            return ;
        }
    }

    // files setup by filesystem_base_c

    init();
}



filesystem_xxdp_c::~filesystem_xxdp_c()
{
    init() ; // free files

    delete changed_blocks ;
    changed_blocks = nullptr ;  // signal to base class destructor
    delete rootdir ;
    rootdir = nullptr ;  // signal to base class destructor
}

// copy filesystem, but without file content
// needed to get a snapshot for change compare
void filesystem_xxdp_c::copy_metadata_to(filesystem_base_c *metadata_copy)
{
    auto _rootdir = dynamic_cast<directory_xxdp_c *>(rootdir) ;
    _rootdir->copy_metadata_to(metadata_copy->rootdir) ;
}


// free / clear all structures,
// set default values from .layout_info
void filesystem_xxdp_c::init()
{
    bitmap.init(this) ;
    /*
    // subsystem same loglevel
    mfd_block_list.log_level_ptr = log_level_ptr ;
    ufd_block_list.log_level_ptr = log_level_ptr ;
    bitmap.block_list.log_level_ptr = log_level_ptr ;
    */

    // set device params
    // blockcount = layout_info.blocks_num;
    // trunc large devices, only 64K blocks addressable = 32MB
    if (blockcount > XXDP_MAX_BLOCKCOUNT)
        blockcount = XXDP_MAX_BLOCKCOUNT;

    preallocated_blockcount = layout_info.prealloc_blocks_num;
    // calced from start to end of preallocated zone?
    monitor_start_block_nr = layout_info.monitor_core_image_start_block_nr ;
    monitor_max_block_count = preallocated_blockcount - monitor_start_block_nr;

    interleave = layout_info.interleave;

    mfd_block_list.init(this);
    // which sort of MFD?
    if (layout_info.mfd2 >= 0) {
        mfd_variety = 1; // 2 blocks
        mfd_block_list.add_empty_block(layout_info.mfd1);
        mfd_block_list.add_empty_block(layout_info.mfd2);
    } else {
        mfd_variety = 2; // single block format
        mfd_block_list.add_empty_block(layout_info.mfd1);
    }
    ufd_block_list.init(this);

    bootblock_filename = make_filename(XXDP_BOOTBLOCK_BASENAME, XXDP_BOOTBLOCK_EXT) ;
    monitor_filename = make_filename(XXDP_MONITOR_BASENAME, XXDP_MONITOR_EXT) ;
    volume_info_filename = make_filename(XXDP_VOLUMEINFO_BASENAME, XXDP_VOLUMEINFO_EXT) ;

    clear_rootdir() ; // clears file list

    struct_changed = false ;
}

/*
 Filesystem parameter for specific drive.
 AC-S866B-MC_CHQFSB0_XXDP+_File_Struct_Doc_Oct84.pdf
 page 9,10
 Modified  by parse of actual disc image.
*/

filesystem_xxdp_c::layout_info_t filesystem_xxdp_c::get_documented_layout_info(enum dec_drive_type_e _drive_type)
{
    layout_info_t result ;
    result.drive_type = _drive_type ;
    result.block_size = 512 ; // for all drives
    result.monitor_block_count = 16 ; // for XXDP+, others?

    switch (_drive_type) {
    case devTU58:
        result.ufd_block_1 = 3 ;
        result.ufd_blocks_num = 4 ;
        result.bitmap_block_1 = 7 ;
        result.bitmap_block_count = 1 ;
        result.mfd1 = 1 ;
        result.mfd2 = 2 ;
        /* DEC defines the XXDP tape to have 511 blocks.
        * But after decades of XXDPDIR and friends,
        * 512 seems to be the standard.
        */
        result.blocks_num = 512 ; // 511,
        result.prealloc_blocks_num = 40 ;
        result.interleave = 1 ;
        result.boot_block_nr = 0 ;
        result.monitor_core_image_start_block_nr = 8 ;
        break ;
    case devRP0456:
        result.ufd_block_1 = 3 ;
        result.ufd_blocks_num = 170 ;
        result.bitmap_block_1 = 173 ;
        result.bitmap_block_count = 50 ;
        result.mfd1 = 1 ;
        result.mfd2 = 2 ;
        result.blocks_num = 48000 ;
        result.prealloc_blocks_num = 255 ;
        result.interleave = 1 ;
        result.boot_block_nr = 0 ;
        result.monitor_core_image_start_block_nr = 223 ;
        break ;
    case devRK035:
        result.ufd_block_1 = 3 ;
        result.ufd_blocks_num = 16 ;
        result.bitmap_block_1 = 4795 ; // ??
        result.bitmap_block_count = 5 ;
        result.mfd1 = 1 ;
        result.mfd2 = 4794 ;
        result.blocks_num = 4800 ;
        result.prealloc_blocks_num = 69 ;
        result.interleave = 5 ;
        result.boot_block_nr = 0 ;
        result.monitor_core_image_start_block_nr = 30 ;
        break ;
    case devRL01:
        result.ufd_block_1 = 24 ;
        result.ufd_blocks_num = 146 ; // 24..169 fiche bad, don north
        result.bitmap_block_1 = 2 ;
        result.bitmap_block_count = 22 ;
        result.mfd1 = 1 ;
        result.mfd2 = -1 ;
        result.blocks_num = 10200 ; // differs from drive_info.get_usable_capacity() ?
        result.prealloc_blocks_num = 200 ;
        result.interleave = 1 ;
        result.boot_block_nr = 0 ;
        result.monitor_core_image_start_block_nr = 170 ;
        break ;
    case devRL02:
        result.ufd_block_1 = 24 ; // actual 2 on XXDP25 image
        result.ufd_blocks_num = 146 ; // 24..169 fiche bad, don north
        result.bitmap_block_1 = 2 ; // actual 24 on XXDP25 image
        result.bitmap_block_count = 22 ;
        result.mfd1 = 1 ;
        result.mfd2 = -1 ;
        result.blocks_num = 20460 ;
        result.prealloc_blocks_num = 200 ;
        result.interleave = 1 ;
        result.boot_block_nr = 0 ;
        result.monitor_core_image_start_block_nr = 170 ;
        break ;
    case devRK067:
        result.ufd_block_1 = 31 ;
        result.ufd_blocks_num = 96 ;
        result.bitmap_block_1 = 2 ;
        result.bitmap_block_count = 29 ;
        result.mfd1 = 1 ;
        result.mfd2 = -1 ;
        result.blocks_num = 27104 ;
        result.prealloc_blocks_num = 157 ;
        result.interleave = 1 ;
        result.boot_block_nr = 0 ;
        result.monitor_core_image_start_block_nr = 127 ;
        break ;
    case devRP023:
        result.ufd_block_1 = 3 ;
        result.ufd_blocks_num = 170 ;
        result.bitmap_block_1 = 173 ;
        result.bitmap_block_count = 2 ;
        result.mfd1 = 1 ;
        result.mfd2 = 2 ;
        result.blocks_num = -1 ; // unknown, bad fiche
        result.prealloc_blocks_num = 255 ;
        result.interleave = 1 ;
        result.boot_block_nr = 0 ;
        result.monitor_core_image_start_block_nr = 223 ;
        break ;
    case devRM:
        result.ufd_block_1 = 52 ;
        result.ufd_blocks_num = 170 ;
        result.bitmap_block_1 = 2 ;
        result.bitmap_block_count = 50 ;
        result.mfd1 = 1 ;
        result.mfd2 = -1 ;
        result.blocks_num = 48000 ;
        result.prealloc_blocks_num = 255 ;
        result.interleave = 1 ;
        result.boot_block_nr = 0 ;
        result.monitor_core_image_start_block_nr = 222 ;
        break ;
    case devRS:
        result.ufd_block_1 = 3 ;
        result.ufd_blocks_num = 4 ;
        result.bitmap_block_1 = 7 ;
        result.bitmap_block_count = 2 ;
        result.mfd1 = 1 ;
        result.mfd2 = 2 ;
        result.blocks_num = 989 ;
        result.prealloc_blocks_num = 41 ;
        result.interleave = 1 ;
        result.boot_block_nr = 0 ;
        result.monitor_core_image_start_block_nr = 9 ;
        break ;
    case devTU56:
        result.ufd_block_1 = 102 ;
        result.ufd_blocks_num = 2 ;
        result.bitmap_block_1 = 104 ;
        result.bitmap_block_count = 1 ;
        result.mfd1 = 100 ;
        result.mfd2 = 101 ;
        result.blocks_num = 576 ;
        result.prealloc_blocks_num = 69 ;
        result.interleave = 5 ;
        result.boot_block_nr = 0 ;
        result.monitor_core_image_start_block_nr = 30 ; // bad fiche, don north
        break ;
    case devRX01:
        result.ufd_block_1 = 3 ;
        result.ufd_blocks_num = 4 ;
        result.bitmap_block_1 = 7 ;
        result.bitmap_block_count = 1 ;
        result.mfd1 = 1 ;
        result.mfd2 = 2 ;
        result.blocks_num = 494 ;
        result.prealloc_blocks_num = 40 ;
        result.interleave = 1 ;
        result.boot_block_nr = 0 ;
        result.monitor_core_image_start_block_nr = 8 ;
        break ;
    case devRX02:
        result.ufd_block_1 = 3 ;
        result.ufd_blocks_num = 16 ;
        result.bitmap_block_count = 4 ;
        result.bitmap_block_1 = 19 ;
        result.mfd1 = 1 ;
        result.mfd2 = 2 ;
        result.blocks_num = 988 ;
        result.prealloc_blocks_num = 55 ;
        result.interleave = 1 ;
        result.boot_block_nr = 0 ;
        result.monitor_core_image_start_block_nr = 23 ;
        break ;
    default:
        FATAL("storageimage_xxdp_c::get_drive_info(): invalid drive") ;
    }
    return result ;
}




// calculate new layout params .layout_info from block_count
// .layout_info must be already initialized for device,
// result: success
void filesystem_xxdp_c::recalc_layout_info(unsigned _blockcount)
{
    xxdp_blocknr_t curblk; // iterates through image

    layout_info.interleave = 1; // not used

    // 1) BOOT
    assert(layout_info.boot_block_nr == 0); // always
    curblk = 1; // always next free block.

    // 2) MFD
    layout_info.mfd1 = 1;
    if (layout_info.mfd2 <= 0)
        curblk++; // MFD1/2
    else {
        layout_info.mfd2 = 2;
        curblk += 2; // MFD1 + MFD2
    }

    // 3) UFD
    layout_info.ufd_block_1 = curblk;
    // # of initial userdir entries
    // from XXDP: 1 UFD block serves ca. 280 file blocks
    // do not assign less than DEC table
    unsigned ufd_blocks_num = needed_blocks(image_partition_size) / 280;
    if (ufd_blocks_num > layout_info.ufd_blocks_num)
        layout_info.ufd_blocks_num = ufd_blocks_num;
    curblk += layout_info.ufd_blocks_num;

    // 4) bitmap size: blocks/8 bytes
    layout_info.bitmap_block_1 = curblk;
    // one bitmap block has entries for 960 blocks
    layout_info.bitmap_block_count = needed_blocks(960, _blockcount);

    curblk += layout_info.bitmap_block_count;

    // 5) monitor_core_image
    layout_info.monitor_core_image_start_block_nr = curblk;

    // accept larger monitor core images on parse, but assert noninal size on layout info
    if (layout_info.monitor_block_count	+ curblk >= layout_info.prealloc_blocks_num)
        ERROR("Layout_info.prealloc_blocks_num not large enough for monitor core") ;
    // normal monitor_core_image size is 16, in RL02 image found as 32 (39 if interleave 5)
//    curblk += 32;
//    layout_info.prealloc_blocks_num = curblk;

    // adapt block flags to new size
    delete changed_blocks ;
    changed_blocks = new boolarray_c(layout_info.blocks_num) ;

}


/*************************************************************
 * low level operators
 *************************************************************/


// true, if any block has his "change" flag set
bool filesystem_xxdp_c::is_blocklist_changed(xxdp_linked_block_list_c *block_list)
{
    assert(changed_blocks) ;
    bool result = false ;
    for (auto it = block_list->begin() ; it != block_list->end() ; ++it) {
        xxdp_blocknr_t block_nr = it->get_block_nr() ;
        result |= changed_blocks->bit_get(block_nr);
    }
    return result ;
}

// use block_nr list of file
bool filesystem_xxdp_c::is_file_blocklist_changed(file_xxdp_c *f)
{
    assert(changed_blocks) ;
    bool result = false ;
    for (unsigned block_nr = 0 ; !result && block_nr < f->block_nr_list.size() ; block_nr++)
        result |= changed_blocks->bit_get(block_nr);
    return result ;
}



// set file->changed from the changed block map
void filesystem_xxdp_c::calc_change_flags()
{
    file_xxdp_c *f ;

    if (changed_blocks == nullptr)
        return;

    struct_changed = false ;

    // linked list of system areas changed?
    struct_changed |= is_blocklist_changed(&mfd_block_list) ;
    struct_changed |= is_blocklist_changed(&bitmap.block_list) ;
    struct_changed |= is_blocklist_changed(&ufd_block_list) ;

    // volume info changed?
    f = dynamic_cast<file_xxdp_c *>(file_by_path.get(volume_info_filename)) ;
    if (f)
        f->changed = struct_changed ;

    // boot block and monitor are files
    for (unsigned i = 0; i < file_count(); i++) {
        f = file_get(i);
        f->changed = is_file_blocklist_changed(f) ;
        assert(changed_blocks) ;
        for (unsigned j = 0; !f->changed && j < f->block_nr_list.size(); j++) {
            unsigned block_nr = f->block_nr_list[j];
            f->changed |= changed_blocks->bit_get(block_nr);
        }
    }
}


/**************************************************************
 * _layout()
 * arrange objects on volume
 **************************************************************/

// quick test, if a new file of "data-size" would fit onto the volume
void filesystem_xxdp_c::layout_test(int data_size)
{
    unsigned ufd_blocks_needed;
    unsigned data_blocks_needed;
    unsigned available_blocks;

    // 1 UFD = 28 files, blocks are 510
    ufd_blocks_needed = needed_blocks(XXDP_UFD_ENTRIES_PER_BLOCK, file_count() + 1);
    // sum up existing files
    data_blocks_needed = 0;
    for (unsigned i = 0; i < file_count(); i++) {
        file_xxdp_c *f = file_get(i);
        if (f->internal)
            continue ;
        int n = needed_blocks(get_block_size()-2, f->file_size);
        // fprintf(stderr,"layout_test(): file %d %s.%s start from %d, needs %d blocks\n", i, f->basename, f->ext,
        // 			 preallocated_blockcount+ data_blocks_needed, n);
        data_blocks_needed += n;
    }
    // space for the new file
    data_blocks_needed += needed_blocks(get_block_size()-2, data_size);

    // we have the space after the preallocated area.
    // It holds all file data and excess UFD blocks
    // the first "ufd_blocks_num (from .layout_info) fit in preallocated space
    assert(blockcount > preallocated_blockcount) ;
    available_blocks = blockcount - preallocated_blockcount;
    if (ufd_blocks_needed > layout_info.ufd_blocks_num)
        data_blocks_needed += ufd_blocks_needed - layout_info.ufd_blocks_num;
    if (data_blocks_needed > available_blocks)
        throw filesystem_exception("layout_test(): Filesystem full");
}

// calculate blocklists for monitor, bitmap,mfd, ufd and files
// total blockcount may be enlarged
void filesystem_xxdp_c::calc_layout()
{
    xxdp_blocknr_t block_nr; // global scan over image

    // mark preallocated blocks in bitmap
    // this covers boot, monitor_core_image, bitmap, mfd and default sized ufd
    memset(bitmap.used, 0, sizeof(bitmap.used));
    for (block_nr = 0; block_nr < preallocated_blockcount; block_nr++)
        bitmap.used[block_nr] = true;

    // Boot block and monitor are fix areas in preallocated image area
    // BITMAP - allocate and mark blocks of bitmap in bitmap itself
    bitmap.block_list.clear() ;
    block_nr = layout_info.bitmap_block_1; // set start
    for (unsigned i = 0; i < layout_info.bitmap_block_count; i++) { // enumerate sequential
        bitmap.block_list.add_empty_block(block_nr) ;
        bitmap.used[block_nr] = true;
        block_nr++ ;
    }

    // MFD
    mfd_block_list.clear();
    if (mfd_variety == 1) {
        mfd_block_list.add_empty_block(layout_info.mfd1); // block 1/2
        mfd_block_list.add_empty_block(layout_info.mfd2); // block 2/2
        bitmap.used[layout_info.mfd1] = true;
        bitmap.used[layout_info.mfd2] = true;
    } else if (mfd_variety == 2) {
        mfd_block_list.add_empty_block(layout_info.mfd1);
        bitmap.used[layout_info.mfd1] = true;
    } else
        FATAL("MFD variety must be 1 or 2");

    // UFD
    // starts in preallocated area, may extend into freespace
    // file_count() excludes internals: boot block and monitor
    {
        unsigned i ;
        unsigned ufd_blocks_num ;
        assert(file_count() >= 2 ) ;
        ufd_blocks_num = needed_blocks(XXDP_UFD_ENTRIES_PER_BLOCK, file_count()-2);
        if (ufd_blocks_num < layout_info.ufd_blocks_num)
            ufd_blocks_num = layout_info.ufd_blocks_num; // static drive info defines minimum
        ufd_block_list.clear();
        // last UFD half filled
        block_nr = layout_info.ufd_block_1;	 // start
        i = 0 ;
        // 1) fill UFD into preallocated space
        while (i < ufd_blocks_num && i < layout_info.ufd_blocks_num) {
            bitmap.used[block_nr] = true;
            ufd_block_list.add_empty_block(block_nr);
            i++ ;
            block_nr++ ;
        }
        // 2) continue UFD into free space, if larger than static device layout defines
        block_nr = preallocated_blockcount; // 1st in free space
        while (i < ufd_blocks_num) {
            bitmap.used[block_nr] = true;
            ufd_block_list.add_empty_block(block_nr);
            i++ ;
            block_nr++ ;
        }
    }
    // block_nr now 1st block behind UFD.

    // FILES

    // files start in free space
    if (block_nr < preallocated_blockcount)
        block_nr = preallocated_blockcount;

    bool overflow = false;
    for (unsigned file_idx = 0; !overflow && file_idx < file_count(); file_idx++) {
        file_xxdp_c *f = file_get(file_idx);
        if (f->internal)
            continue ;
        // amount of 510 byte blocks
        f->last_block_nr = f->start_block_nr = block_nr ;

        f->block_count = needed_blocks(get_block_size()-2, f->file_size);
        assert(f->block_count > 0) ;
        f->block_nr_list.resize(f->block_count) ;
        // fprintf(stderr,"layout(): file %d %s.%s start from %d, needs %d blocks\n", i, f->basename, f->ext, block_nr, n);
        for (unsigned j = 0; !overflow && j < f->block_count; j++) {
            if (j >= XXDP_MAX_BLOCKS_PER_LIST) {
                throw filesystem_exception("Filesystem overflow. File %s.%s too large, uses more than %d blocks",
                                           f->basename.c_str(), f->ext.c_str(), XXDP_MAX_BLOCKS_PER_LIST);
                overflow = true;
            } else if (block_nr >= blockcount) {
                throw filesystem_exception("File system overflow, can hold max %d blocks.", blockcount);
                overflow = true;
            } else {
                f->last_block_nr = block_nr ; // end block is moving upwards
                bitmap.used[block_nr] = true;
                f->block_nr_list.at(j) = block_nr++;  // at() has range check
            }
        }
        if (overflow)
            throw filesystem_exception("File system overflow");
    }

    // expand file system size if needed.
    // later check wether physical image can be expanded.
    if (block_nr >= blockcount)
        throw filesystem_exception("File system overflow");
}




/**************************************************************
 * parse()
 * convert byte array of image into logical objects
 **************************************************************/

// read the "Master File Directory" MFD, produce MFD,
// read Bitmap and UFD block lists
void filesystem_xxdp_c::parse_mfd_load_bitmap_ufd()
{
    xxdp_blocknr_t  mfd_start_block_nr = layout_info.mfd1 ;

    mfd_block_list.load_from_image(mfd_start_block_nr);
//    mfd_block_list.print_diag(cout, "mfd_block_list") ;
    if (mfd_block_list.size() == 2) {
        // 2 blocks. first predefined, fetch 2nd from image
        // Prefer MFD data over linked list scan, why?
        if (mfd_variety != 1)
            WARNING("MFD is 2 blocks, variety 1 expected, but variety %d defined", mfd_variety);
        xxdp_linked_block_c *mfd_block1 = mfd_block_list.get_block_by_nr(mfd_start_block_nr) ;
        interleave = mfd_block1->get_word_at_word_offset(1);

        // build bitmap blocklist from MFD, do not read data yet
        // word[2] = bitmap start block, word[3] = pointer to bitmap #1, always the same?
        // a 0 terminated list of bitmap blocks is in MFD1, word 2,3,...
        // DEC prefers MFD table over linked list scan, speed?
        xxdp_blocknr_t bitmap_start_block_nr = mfd_block1->get_word_at_word_offset(2) ;
        bitmap.block_list.load_from_image(bitmap_start_block_nr) ; // load via block links
        bitmap.block_list.verify() ;
        // check linked bitmap block numbers against MFD1 table
        unsigned n = 0; // count bitmap blocks
        for (auto it = bitmap.block_list.begin(); it != bitmap.block_list.end(); ++it) {
            xxdp_linked_block_c *bitmap_block = &(*it) ;
            assert(n + 3 < 255);
            xxdp_blocknr_t mfd1_bitmap_block_nr = mfd_block1->get_word_at_word_offset(n + 3);
            assert(mfd1_bitmap_block_nr == bitmap_block->get_block_nr()) ;
            n++ ;
        }
        /*
                n = 0; // count bitmap blocks
                bitmap.block_list.clear() ;
                do {
                    assert(n + 3 < 255);
                    blknr = mfd_block_list.block_get_word(mfd_start_block_nr, n + 3);
                    if (blknr > 0) {
        				// load new bitmap block
        				bitmap.block_list.add_from_image(blknr) ;
                        assert(bitmap.block_list[n].start_block_nr == blknr);
                        n++;
                    }
                } while (blknr > 0);
                assert(bitmap.block_list.size() == n);
        */

        // build UFD block list from MFD2
        assert(mfd_block_list.size() > 1) ; // was tested to be 2

        xxdp_blocknr_t mfd2_start_block_nr = mfd_block1->get_word_at_word_offset(0) ; // link
        xxdp_linked_block_c *mfd_block2 = mfd_block_list.get_block_by_nr(mfd2_start_block_nr) ;
        // Get start of User File Directory from MFD2, word 2, then scan linked list
        xxdp_blocknr_t ufd_start_block_nr = mfd_block2->get_word_at_word_offset(2) ;
        ufd_block_list.load_from_image(ufd_start_block_nr) ;
        ufd_block_list.verify() ;
    } else if (mfd_block_list.size() == 1) {
        // var 2: "MFD1/2" RL01/2 ?
        if (mfd_variety != 2)
            WARNING( "MFD is 1 blocks, variety 2 expected, but variety %d defined",
                     mfd_variety);
        xxdp_linked_block_c *mfd_block = mfd_block_list.get_block_by_nr(mfd_start_block_nr) ;

        // Build UFD block list
        xxdp_blocknr_t ufd_start_block_nr = mfd_block->get_word_at_word_offset(1);
        uint16_t ufd_block_count = mfd_block->get_word_at_word_offset(2);

        ufd_block_list.load_from_image(ufd_start_block_nr) ;
//		ufd_block_list.print_diag(cout, "ufd_block_list") ;
        // verify len
        if (ufd_block_count != ufd_block_list.size())
            WARNING( "UFD block count is %u, but %d in MFD1/2",
                     ufd_block_list.size(), ufd_block_count);
        // best choice is len of disk list

        // Build Bitmap block list
        xxdp_blocknr_t bitmap_start_block_nr = mfd_block->get_word_at_word_offset(3);
        uint16_t bit_block_count = mfd_block->get_word_at_word_offset(4);
        bitmap.block_list.load_from_image(bitmap_start_block_nr) ;
//		bitmap.block_list.print_diag(cout, "bitmap.block_list") ;

        // verify len
        if (bit_block_count != bitmap.block_list.size())
            WARNING("Bitmap block count is %u, but %d in MFD1/2", bitmap.block_list.size(), bit_block_count);

        // total num of blocks
        unsigned n = mfd_block->get_word_at_word_offset(7);
        if (n != blockcount)
            WARNING("Device blockcount is %u in layout_info, but %d in MFD1/2", blockcount, n);
        blockcount = n ;

        preallocated_blockcount = mfd_block->get_word_at_word_offset(8);
        if (preallocated_blockcount != layout_info.prealloc_blocks_num)
            WARNING("Device preallocated blocks are %u in layout_info, but %d in MFD1/2",
                    layout_info.prealloc_blocks_num, preallocated_blockcount);

        interleave =  mfd_block->get_word_at_word_offset(9);
        if (interleave != layout_info.interleave)
            WARNING("Device interleave is %u in layout_info, but %d in MFD1/2", layout_info.interleave, interleave);

        monitor_start_block_nr =  mfd_block->get_word_at_word_offset(11);
        if (monitor_start_block_nr != layout_info.monitor_core_image_start_block_nr)
            WARNING("Monitor core start is %u in layout_info, but %d in MFD1/2",
                    layout_info.monitor_core_image_start_block_nr, monitor_start_block_nr);
        // last monitor_core_image block = preallocated_blockcount
        monitor_max_block_count = preallocated_blockcount - monitor_start_block_nr ;
        if (monitor_max_block_count != layout_info.monitor_block_count)
            WARNING("Monitor core len %d blocks, should be %u",monitor_max_block_count,
                    layout_info.monitor_block_count) ;


        WARNING("Position of bad block file not yet evaluated");
    } else
        FATAL("Invalid block count in MFD: %d", mfd_block_list.size());
}

// bitmap blocks loaded, produce "used[]" flag array
void filesystem_xxdp_c::parse_bitmap()
{
    xxdp_blocknr_t image_block_nr = 0; // enumerates the block flags
    // assume consecutive bitmap blocks encode consecutive block numbers
    // what about map number?
    for (unsigned i=0 ; i < bitmap.block_list.size(); i++) {
        xxdp_linked_block_c *bitmap_block = &(bitmap.block_list[i]) ;

        // verify link to this bitmap bitmap_block
        xxdp_blocknr_t bitmap_number = bitmap_block->get_word_at_word_offset(1);
        assert(bitmap_number == i+1) ; // number of this bitmap block starting at 1

        uint16_t bitmap_used_words_count = bitmap_block->get_word_at_word_offset(2);  // always 60 ?
        assert(bitmap_used_words_count == XXDP_BITMAP_WORDS_PER_MAP);

        // verify link to 1st bitmap bitmap_block
        xxdp_blocknr_t bitmap_first_block_nr = bitmap_block->get_word_at_word_offset(3);
        assert(bitmap_first_block_nr == bitmap.block_list[0].get_block_nr()) ;

        // hexdump(flog, blocknr2offset(map_blknr), 512, "Block %u = bitmap %u", map_blknr, i);
        for (unsigned j = 0; j < bitmap_used_words_count; j++) {
            uint16_t bitmap_flags = bitmap_block->get_word_at_word_offset(j + 4);
            // 16 flags per word. LSB = lowest blocknr
            for (unsigned k = 0; k < 16; k++) {
                assert(image_block_nr == (i * XXDP_BITMAP_WORDS_PER_MAP + j) * 16 + k);
                if (bitmap_flags & (1 << k))
                    bitmap.used[image_block_nr] = true;
                else
                    bitmap.used[image_block_nr] = false;
                image_block_nr++;
            }
        }
    }
    // image_block_nr is now count of defined blocks
    // bitmap.blockcount = image_block_nr;
}

// read block[start] ... block[start+blockcount-1] into data[]


// parse filesystem special blocks to new file
void filesystem_xxdp_c::parse_internal_contiguous_file(    string _basename, string _ext,
        xxdp_blocknr_t _start_blocknr, xxdp_blocknr_t _block_count)
{
    string fname = make_filename(_basename, _ext) ;
    file_base_c* fbase = file_by_path.get(fname);
    file_xxdp_c* f = dynamic_cast<file_xxdp_c*>(fbase);

    assert(f == nullptr) ;
    f = new file_xxdp_c(); // later own by rootdir
    f->internal = true ;
    f->is_contiguous_file = true ;
    f->basename = _basename ;
    f->ext = _ext ;
    f->start_block_nr = _start_blocknr;
    f->block_count = _block_count;
    f->last_block_nr = _start_blocknr + _block_count - 1;
    f->readonly = true ;
    rootdir->add_file(f); //  before parse-stream

    unsigned data_size = get_block_size() * _block_count;
    image_partition->get_bytes(f, blocknr2offset(_start_blocknr), data_size) ;
    f->file_size = f->size() ; // size from inherited bytebuffer
    f->host_path = f->get_host_path() ;
}


// UFD blocks loaded, produce filelist
void filesystem_xxdp_c::parse_ufd()
{
//    xxdp_blocknr_t blknr; // enumerates the directory blocks
    for (unsigned i = 0; i < ufd_block_list.size(); i++) {
        xxdp_linked_block_c *ufd_block = &(ufd_block_list[i]);
//        blknr = ufd_block_list[i];
        // hexdump(flog, blocknr2offset(blknr), 512, "Block %u = UFD block %u", blknr, i);
        // 28 dir entries per block
        for (unsigned j = 0; j < XXDP_UFD_ENTRIES_PER_BLOCK; j++) {
            uint32_t file_entry_start_word_nr = 1 + j * XXDP_UFD_ENTRY_WORDCOUNT; // point to start of file entry in block
            uint16_t w;
            // filename: 2*3 radix50 chars
            w = ufd_block->get_word_at_word_offset(file_entry_start_word_nr + 0);
            if (w == 0)
                continue; // invalid entry
            if (file_count() >= XXDP_MAX_FILES_PER_IMAGE)
                throw filesystem_exception("Filesystem overflow. XXDP UFD read: more than %d files!", XXDP_MAX_FILES_PER_IMAGE);

            // create file entry
            file_xxdp_c *f = new file_xxdp_c();
            f->changed = false;
            f->internal = false;

            // basename: 6 chars
            f->basename.assign(rad50_decode(w)) ;

            w = ufd_block->get_word_at_word_offset(file_entry_start_word_nr + 1);
            f->basename.append(rad50_decode(w)) ;

            // extension: 3 chars
            w = ufd_block->get_word_at_word_offset(file_entry_start_word_nr + 2);
            f->ext.assign(rad50_decode(w));

            w = ufd_block->get_word_at_word_offset(file_entry_start_word_nr + 3);
            f->modification_time = dos11date_decode(w);

            // start block, importn blocklist in parse_file_data()
            f->start_block_nr = ufd_block->get_word_at_word_offset(file_entry_start_word_nr + 5);

            // check: filelen?
            f->block_count = ufd_block->get_word_at_word_offset(file_entry_start_word_nr + 6);

            // check: lastblock?
            f->last_block_nr = ufd_block->get_word_at_word_offset(file_entry_start_word_nr + 7);

            rootdir->add_file(f) ;  // save, owned by directory now

            f->host_path = f->get_host_path() ; // after insert into filesystem!
        }
    }
}

// load and allocate file data from blocklist
// data is read in 510 byte chunks, actual size not known
void filesystem_xxdp_c::parse_file_data(file_xxdp_c *f)
{
    // use temporary linked data blocls to load from image
    xxdp_linked_block_list_c block_list ;
    block_list.init(this) ;
    block_list.load_from_image(f->start_block_nr) ;
    // save block numbers
    block_list.get_block_nr_list(&(f->block_nr_list)) ;

    // fill block list content to stream byte buffer
    block_list.write_to_file_buffer(f) ;
}

// fill the pseudo file with textual volume information
// it never changes
void filesystem_xxdp_c::parse_volumeinfo()
{
    file_xxdp_c* fout = dynamic_cast<file_xxdp_c*>(file_by_path.get(volume_info_filename));
    if (fout == nullptr) {
        fout = new file_xxdp_c(); // later owned by rootdir
        fout->internal = true ;
        fout->basename = XXDP_VOLUMEINFO_BASENAME ;
        fout->ext  = XXDP_VOLUMEINFO_EXT ;
        fout->start_block_nr = 0; // not needed
        fout->block_count = 0 ;
        fout->readonly = true ;

        rootdir->add_file(fout); // before stream creation
        fout->host_path = fout->get_host_path() ;  // after insert into filesystem!
    }

    string text_buffer ; // may grow large, but data kept on heap

    char line[1024];

    sprintf(line, "# %s - info about XXDP volume on %s device #%u.\n",
            volume_info_filename.c_str(), drive_info.device_name.c_str(), drive_unit);
    text_buffer.append(line);

    time_t t = time(NULL);
    struct tm tm = *localtime(&t);
    sprintf(line, "# Produced by QUnibone at %d-%d-%d %d:%d:%d\n", tm.tm_year + 1900,
            tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
    text_buffer.append(line);

    sprintf(line, "\n# Blocks on device\nblockcount=%d (XXDP doc says %d)\n", blockcount, layout_info.blocks_num);
    text_buffer.append(line);

    sprintf(line, "prealloc_blocks_num=%d (XXDP doc says %d)\n", (unsigned) preallocated_blockcount, layout_info.prealloc_blocks_num);
    text_buffer.append(line);
    sprintf(line, "interleave=%d (XXDP doc says %d)\n", interleave, layout_info.interleave);
    text_buffer.append(line);
    sprintf(line, "boot_block=%d\n", layout_info.boot_block_nr);
    text_buffer.append(line);
    sprintf(line, "monitor_block=%d (XXDP doc says %d)\n", (unsigned) monitor_start_block_nr, layout_info.monitor_core_image_start_block_nr);
    text_buffer.append(line);
    sprintf(line, "monitor_blockcount=%d (all remaining preallocated blocks, XXDP doc says %d) \n",
            (unsigned)  monitor_max_block_count, layout_info.monitor_block_count);
    text_buffer.append(line);

    sprintf(line, "\n# Master File Directory\n") ;
    text_buffer.append(line);
    sprintf(line, "variety = %d (Var1: MFD1+MFD2, Var2: MFD1/2, XXDP doc says %d\n",
            mfd_variety, layout_info.mfd2 < 0 ? 2 : 1) ;
    text_buffer.append(line);
    sprintf(line, "mfd1=%d\n", layout_info.mfd1); // = mfd_start_block_nr
    text_buffer.append(line);
    if (layout_info.mfd2 > 0) {
        sprintf(line, "mfd2=%d\n", layout_info.mfd2);
        text_buffer.append(line);
    }
    sprintf(line, "\n# Bitmap of used blocks:\nbitmap_block_1=%d (XXDP doc says %d)\n",
            (unsigned)bitmap.block_list[0].get_block_nr(), layout_info.bitmap_block_1);
    text_buffer.append(line);
    sprintf(line, "bitmaps_num=%d (XXDP doc says %d)\n",  bitmap.block_list.size(), layout_info.bitmap_block_count);
    text_buffer.append(line);

    sprintf(line, "\n# User File Directory:\nufd_block_1=%d (XXDP doc says %d)\n",
            (unsigned)ufd_block_list[0].get_block_nr(),	layout_info.ufd_block_1);
    text_buffer.append(line);
    sprintf(line, "ufd_blocks_num=%d (XXDP doc says %d)\n",
            ufd_block_list.size(), layout_info.ufd_blocks_num);
    text_buffer.append(line);

    for (unsigned file_idx = 0; file_idx < file_count(); file_idx++) {
        file_xxdp_c *f = file_get(file_idx);
        if (f->internal)
            continue ;
        sprintf(line, "\n# File %2d \"%s.%s\".", file_idx, f->basename.c_str(), f->ext.c_str());
        text_buffer.append(line);
        assert(f->block_nr_list.size() > 0) ;
        sprintf(line, " Data %u = 0x%X bytes, start block %d @ 0x%X.", f->file_size,
                f->file_size, f->start_block_nr,
                f->start_block_nr * get_block_size());
        text_buffer.append(line);
    }
    text_buffer.append("\n");

    fout->set_data(&text_buffer) ;
    fout->file_size = fout->size() ; // stream -> file

    // VOLUM INF is "changed", if home block or directories changed
    fout->changed = struct_changed;
}





// analyse the image, build filesystem data structure
// parameters already set by _reset()
// return: true = OK
void filesystem_xxdp_c::parse()
{
    std::exception_ptr eptr;

    // events in the queue references streams, which get invalid on re-parse.
    assert(event_queue.empty()) ;

    init();

    try {
        // parse structure info first, will update lay_out info
        parse_mfd_load_bitmap_ufd();
        parse_bitmap();
        parse_ufd();

        // read bootblock 0 and create file. may consist of 00's
        parse_internal_contiguous_file(XXDP_BOOTBLOCK_BASENAME, XXDP_BOOTBLOCK_EXT, layout_info.boot_block_nr, 1);
        // read monitor and create file: from defined start until end of preallocated area, about 32
        parse_internal_contiguous_file(XXDP_MONITOR_BASENAME, XXDP_MONITOR_EXT,
                                       layout_info.monitor_core_image_start_block_nr, monitor_max_block_count);

        // read data for all user files
        for (unsigned i = 0; i < file_count(); i++)
            if (!file_get(i)->internal)
                parse_file_data(file_get(i));
    }
    catch (filesystem_exception &e) {
        eptr = std::current_exception() ;
    }

    calc_change_flags();

    //  data now stable, generate internal volume info text last
    parse_volumeinfo() ;

//    print_directory(stdout) ;

    if (eptr != nullptr)
        std::rethrow_exception(eptr);
}


/**************************************************************
 * render
 * create an binary image from logical datas structure
 **************************************************************/

// write the bitmap words of all used[] blocks, to bitmap and to image
// block_list[] already allocated and linked by calc_layout()
void filesystem_xxdp_c::render_bitmap()
{
    assert(bitmap.block_list.size() > 0) ;
    xxdp_blocknr_t first_block_nr = bitmap.block_list[0].get_block_nr();
    xxdp_blocknr_t image_block_nr;

    for (image_block_nr = 0; image_block_nr < blockcount; image_block_nr++) {
        // which bitmap block resides the flag in?
        int bitmap_block_idx = image_block_nr / (XXDP_BITMAP_WORDS_PER_MAP * 16); // # in list
        xxdp_linked_block_c *bitmap_block = &bitmap.block_list[bitmap_block_idx];
        // bit index of this image block in whole bitmap block
        int bitmap_block_bit_nr = image_block_nr % (XXDP_BITMAP_WORDS_PER_MAP * 16);

        // set metadata for the whole bitmap block only if first flag is processed
        if (bitmap_block_bit_nr == 0) {
            bitmap_block->set_word_at_word_offset(1, bitmap_block_idx + 1); // "map number": enumerates map blocks
            bitmap_block->set_word_at_word_offset(2, XXDP_BITMAP_WORDS_PER_MAP); // always 060
            bitmap_block->set_word_at_word_offset(3, first_block_nr); // "link to first map"
        }

        // set a single block flag
        if (bitmap.used[image_block_nr]) {
            // word and bit in this block containing the flag
            int bitmap_block_flags_nr = bitmap_block_bit_nr / 16;
            int bitmap_block_flags_bitpos = bitmap_block_bit_nr % 16;
            uint16_t bitmap_flags = bitmap_block->get_word_at_word_offset(bitmap_block_flags_nr + 4);
            bitmap_flags |= (1 << bitmap_block_flags_bitpos);
            bitmap_block->set_word_at_word_offset(bitmap_block_flags_nr + 4, bitmap_flags);
        }
    }
    bitmap.block_list.write_to_image() ;
}

// block_list[] already allocated and linked by calc_layout()
void filesystem_xxdp_c::render_mfd()
{
    xxdp_linked_block_c *mfd_block ;
    if (mfd_variety == 1) {
        // two mfd_block MFD1, MFD2
        assert(mfd_block_list.size() == 2);
        // write MFD1
        mfd_block = &mfd_block_list[0];
        mfd_block->set_word_at_word_offset(1, interleave);
        xxdp_blocknr_t bitmap_start_block_nr = bitmap.block_list[0].get_block_nr();
        mfd_block->set_word_at_word_offset(2, bitmap_start_block_nr);
        // direct list of bitmap blocks
        unsigned n = bitmap.block_list.size();
        assert(n <= 251); // space for 256 - 3 - 2 = 251 bitmap blocks
        for (unsigned i = 0; i < n; i++)
            mfd_block->set_word_at_word_offset(i + 3, bitmap.block_list[i].get_block_nr());
        // terminate list
        mfd_block->set_word_at_word_offset(n + 3, 0);

        // write MFD2
        mfd_block = &mfd_block_list[1];
        mfd_block->set_word_at_word_offset(1, 0401); // UIC[1,1]
        xxdp_blocknr_t ufd_start_block_nr = ufd_block_list[0].get_block_nr();
        mfd_block->set_word_at_word_offset(2, ufd_start_block_nr); // start of UFDs
        mfd_block->set_word_at_word_offset(3, XXDP_UFD_ENTRY_WORDCOUNT);
        mfd_block->set_word_at_word_offset(4, 0);
    } else if (mfd_variety == 2) {
        // MFD1/2
        assert(mfd_block_list.size() == 1);
        mfd_block = &mfd_block_list[0];
        xxdp_blocknr_t ufd_start_block_nr = ufd_block_list[0].get_block_nr();
        mfd_block->set_word_at_word_offset(1, ufd_start_block_nr); // ptr to 1st UFD blk
        mfd_block->set_word_at_word_offset(2, ufd_block_list.size());
        xxdp_blocknr_t bitmap_start_block_nr = bitmap.block_list[0].get_block_nr();
        xxdp_blocknr_t bitmap_block_count = bitmap.block_list.size();
        mfd_block->set_word_at_word_offset(3, bitmap_start_block_nr); // ptr to bitmap
        mfd_block->set_word_at_word_offset(4, bitmap_block_count);
        mfd_block->set_word_at_word_offset(5, mfd_block->get_block_nr()); // ptr to self
        mfd_block->set_word_at_word_offset(6, 0);
        mfd_block->set_word_at_word_offset(7, blockcount); // # of supported blocks
        mfd_block->set_word_at_word_offset(8, preallocated_blockcount);
        mfd_block->set_word_at_word_offset(9, interleave);
        mfd_block->set_word_at_word_offset(10, 0);
        mfd_block->set_word_at_word_offset(11, monitor_start_block_nr); // 1st of monitor_core_image core img
        mfd_block->set_word_at_word_offset(12, 0);
        // bad sector position: needs to be define in RADI for each device
        mfd_block->set_word_at_word_offset(13, 0);
        mfd_block->set_word_at_word_offset(14, 0);
        mfd_block->set_word_at_word_offset(15, 0);
        mfd_block->set_word_at_word_offset(16, 0);
    } else
        FATAL("MFD variety must be 1 or 2");
    mfd_block_list.write_to_image() ;
}

// block_list[] already allocated and linked by calc_layout()
void filesystem_xxdp_c::render_ufd()
{
    for (unsigned file_idx = 0; file_idx < file_count(); file_idx++) {
        file_xxdp_c *f = file_get(file_idx);
        if (f->internal)
            continue ;

        xxdp_blocknr_t ufd_block_nr = layout_info.ufd_block_1 + (file_idx / XXDP_UFD_ENTRIES_PER_BLOCK);
        xxdp_linked_block_c *ufd_block = &ufd_block_list[ufd_block_nr] ;

        // word nr of cur entry in cur block. skip link word.
        int ufd_word_offset = 1 + (file_idx % XXDP_UFD_ENTRIES_PER_BLOCK) * XXDP_UFD_ENTRY_WORDCOUNT;

        char buff[80];
        // filename chars 0..2
        strncpy(buff, f->basename.c_str(), 3);
        buff[3] = 0;
        ufd_block->set_word_at_word_offset(ufd_word_offset + 0, rad50_encode(buff));
        // filename chars 3..5
        if (strlen(f->basename.c_str()) < 4)
            buff[0] = 0;
        else
            strncpy(buff, f->basename.c_str() + 3, 3);
        buff[3] = 0;
        ufd_block->set_word_at_word_offset(ufd_word_offset + 1, rad50_encode(buff));
        ufd_block->set_word_at_word_offset(ufd_word_offset + 2, rad50_encode((char *)f->ext.c_str()));
        ufd_block->set_word_at_word_offset(ufd_word_offset + 3, dos11date_encode(f->modification_time));
        ufd_block->set_word_at_word_offset(ufd_word_offset + 4, 0); // ACT-11 logical end
        ufd_block->set_word_at_word_offset(ufd_word_offset + 5, f->start_block_nr); // 1st block
        ufd_block->set_word_at_word_offset(ufd_word_offset + 6, f->block_count); // file length in blocks
        ufd_block->set_word_at_word_offset(ufd_word_offset + 7, f->last_block_nr); // last block
        ufd_block->set_word_at_word_offset(ufd_word_offset + 8, 0); // ACT-11 logical 52
    }
    ufd_block_list.write_to_image() ;
}


// simple byte_buffer to whole image block sequence
// side effect: file size() rounded up to whole blocks
// 00s automatically filled in unused bytes, f.size() maybe 0
void filesystem_xxdp_c::render_contiguous_file_data(file_xxdp_c *f)
{
    unsigned round_up_size = get_block_size() * needed_blocks(get_block_size(), f->size()) ;
    assert(round_up_size >= f->size()) ;
    f->set_size(round_up_size) ; // new space set to zero_byte_val
    f->image_position = get_block_size() * f->start_block_nr ;
    image_partition->set_bytes(f) ;
}

// write file->data[] into linked blocks of pre-calced block_nr_list
void filesystem_xxdp_c::render_file_data()
{
    // read data for all user files
    for (unsigned i = 0; i < file_count(); i++) {
        file_xxdp_c *f = file_get(i) ;
        if (f->internal)
            continue ;
        assert(!f->is_contiguous_file) ; // boot block and monitor are internal

        if (f->block_count != f->block_nr_list.size())
            WARNING("XXDP UFD read: file %s.%s: saved file size is %d, blocklist len is %d.\n",
                    f->basename.c_str(), f->ext.c_str(), f->block_count, f->block_nr_list.size());

        // write byte stream to temporary linked block list
        xxdp_linked_block_list_c block_list ;
        block_list.init(this) ;
        block_list.add_empty_blocks(&f->block_nr_list) ;
        block_list.load_from_file_buffer(f) ;
        block_list.write_to_image() ;
    }
}

// write filesystem into image
// Assumes all file data and blocklists are valid
// return: 0 = OK
void filesystem_xxdp_c::render()
{
    unsigned needed_size = blockcount * get_block_size();

    // calc blocklists and sizes
    calc_layout() ; // throws

    if (needed_size > image_partition_size)
        // more bytes needed than image provides. Expand?
        throw filesystem_exception("Image only %d bytes large, filesystem needs %d *%d = %d.", image_partition_size,
                                   blockcount, get_block_size(), needed_size);

    // write boot block and monitor, if file exist
    // bootblock and monitor: start and block_count set by calc_layout()
    // 00s automatically filled in unused bytes, size() maybe 0
    file_xxdp_c* bootblock = dynamic_cast<file_xxdp_c*>(file_by_path.get(bootblock_filename));
    if (bootblock) {
        bootblock->start_block_nr = layout_info.boot_block_nr;
        if (bootblock->size() != get_block_size())
            throw filesystem_exception("bootblock has illegal size of %d bytes.", bootblock->size());
        render_contiguous_file_data(bootblock) ;
    } else
        image_partition->set_zero(layout_info.boot_block_nr, get_block_size()) ; // clear area

    file_xxdp_c* monitor = dynamic_cast<file_xxdp_c*>(file_by_path.get(monitor_filename));
    if (monitor) {
        monitor->start_block_nr = monitor_start_block_nr;
        if (monitor->size() > monitor_max_block_count * get_block_size())
            throw filesystem_exception("monitor has illegal size of %d bytes.", monitor->size());
        render_contiguous_file_data(monitor);
    } else
        image_partition->set_zero(monitor_start_block_nr * get_block_size(), monitor_max_block_count * get_block_size()) ; // clear area

    render_bitmap();
    render_mfd();
    render_ufd();

    render_file_data();

    parse_volumeinfo() ;

}


/**************************************************************
 * FileAPI
 * add / get files in logical data structure
 **************************************************************/


void filesystem_xxdp_c::import_host_file(file_host_c *host_file)
{
    bool block_ack_event = true ; // do not feed changes back to host
    // false: changes are re-sent to the host. necessary for files like VOLUME.INF,
    // which change independently and whose changes must sent to the host.


    // XXDP has no subdirectories, so it accepts only plain host files from the rootdir
    // report file $VOLUME INFO not be read back
    if (dynamic_cast<directory_host_c*>(host_file) != nullptr)
        return ; // host directory
    if (host_file->parentdir == nullptr)
        return ;  // host root directory
    if (host_file->parentdir->parentdir != nullptr)
        return ; // file in host root subdirectory


    string host_fname = host_file->get_filename(); // XXDP:  path == name

    // make filename.extension to "FILN  .E  "
    string _basename, _ext;

    filename_from_host(&host_fname, &_basename, &_ext);

    string filename = make_filename(_basename, _ext) ;
    // duplicate file name? Likely! because of trunc to six letters
    file_xxdp_c *f = dynamic_cast<file_xxdp_c *>(file_by_path.get(filename)) ; // already hashed?
    if (f != nullptr) {
        DEBUG(printf_to_cstr("XXDP: Ignore \"create\" event for existing filename/stream %s.%s",
                             _basename.c_str(), _ext.c_str())) ;
        return ;
    }

    host_file->data_open(/*write*/ false) ; // need file size early

    bool is_internal_contiguous = false ;
    if (_basename == XXDP_BOOTBLOCK_BASENAME && _ext == XXDP_BOOTBLOCK_EXT) {
        assert(filename == bootblock_filename) ;
        is_internal_contiguous = true ;
        if (host_file->file_size != get_block_size())
            throw filesystem_exception("Boot block not %d bytes", get_block_size());
    } else if (_basename == XXDP_MONITOR_BASENAME && _ext == XXDP_MONITOR_EXT) {
        assert(filename == monitor_filename) ;
        is_internal_contiguous = true ;
        if (host_file->file_size >  monitor_max_block_count * get_block_size())
            throw filesystem_exception("Monitor block too big, has %d bytes, max %d", host_file->file_size,
                                       monitor_max_block_count * get_block_size());
    } else if (_basename == XXDP_VOLUMEINFO_BASENAME && _ext == XXDP_VOLUMEINFO_EXT) {
        return ; // VOLUME.INF only DEC->host
    }

    // check wether a new user file of "data_size" bytes would fit onto volume
    // recalc filesystem parameters
    try {
        layout_test(is_internal_contiguous ? 0 : host_file->file_size) ;
    } catch (filesystem_exception &e) {
        throw filesystem_exception("Disk full, file \"%s\" with %d bytes too large", host_fname.c_str(), host_file->file_size);
    }


    // now insert
    f = new file_xxdp_c();
    f->changed = false;
    f->internal = is_internal_contiguous;
    f->is_contiguous_file = is_internal_contiguous;
    f->host_path = host_file->path ;
    f->set_data(&host_file->data, host_file->file_size);
    f->file_size = f->size() ; // from inherited stream
    f->block_count = needed_blocks(f->size());
    f->basename = _basename ;
    f->ext = _ext ;
    f->modification_time = host_file->modification_time;

    // only range 1970..1999 allowed
    if (f->modification_time.tm_year < 70)
        f->modification_time.tm_year = 70;
    else if (f->modification_time.tm_year > 99)
        f->modification_time.tm_year = 99;

    rootdir->add_file(f) ; // add, now owned by rootdir

    host_file->data_close() ;

    if (block_ack_event)
        ack_event_filter.add(host_file->path) ;
}


void filesystem_xxdp_c::delete_host_file(string host_path)
{
    // build XXDP name and stream code
    string host_dir, host_fname, _basename, _ext ;

    split_path(host_path, &host_dir, &host_fname, nullptr, nullptr) ;
    if (host_dir != "/")
        // ignore stuff from host subdirectories
        return ;

    filename_from_host(&host_fname, &_basename, &_ext);
    string filename = make_filename(_basename, _ext) ;

    file_xxdp_c *f = dynamic_cast<file_xxdp_c *>(file_by_path.get(filename)) ; // already hashed?

    if (f == nullptr) {
        DEBUG(printf_to_cstr("XXDP: ignore \"delete\" event for missing file %s.", host_fname.c_str()));
        return ;
    }

    // boot block and monitor ar regular files, volumne info is virtual
    if (_basename == XXDP_VOLUMEINFO_BASENAME && _ext == XXDP_VOLUMEINFO_EXT) {
        return ;
    }

    rootdir->remove_file(f) ;

    ack_event_filter.add(host_path) ;

}



file_xxdp_c *filesystem_xxdp_c::file_get(int fileidx)
{
    file_xxdp_c *f ;
    if (fileidx >= 0 && fileidx < (int)file_count()) {
        // regular file. Must've been added with rootdir->add_file()
        f = dynamic_cast< file_xxdp_c *> (rootdir->files[fileidx]) ;
        assert(f) ;
    } else
        return nullptr; // not a valid file idx

    return f ;
}


// result ist basename.ext, without spaces
// "filname" and "ext" contain components WITH spaces, if != NULL
// "bla.foo.c" => "BLA.FO", "C	", result = "BLA.FO.C"
// "bla" => "BLA."
string filesystem_xxdp_c::filename_from_host(string *hostfname, string *result_basename, string *result_ext)
{
    string pathbuff = *hostfname ;

    // upcase and replace forbidden characters
    for (unsigned i = 0 ; i < pathbuff.length() ; i++) {
        char c ;
        switch(c = pathbuff[i]) {
        case '_' :
            c = ' ' ;
            break ;
        case 'a' ... 'z':
            c = toupper(c) ;
            break ;
        case 'A' ... 'Z':
        case '$':
        case '.':
        case '0' ... '9':
            c = c ;
            break ;
        default:
            c = '%' ;
        }
        pathbuff[i] = c ;
    }

    // make it 6.3. can use Linux function
    string _basename, _ext ;
    split_path(pathbuff, nullptr, nullptr, &_basename, &_ext) ;
    _ext = _ext.substr(0, 3) ;
    trim(_ext) ;
    _basename = _basename.substr(0, 6) ;
    trim(_basename) ;



    if (result_basename != nullptr)
        *result_basename = _basename;
    if (result_ext != nullptr)
        *result_ext = _ext;

    // with "." on empty extension "FILE."
    return make_filename(_basename, _ext) ;
}



// sort files in rootdir according to order,
// set by "sort_add_group_pattern()"
void filesystem_xxdp_c::sort()
{
    // non recursive
    filesystem_base_c::sort(rootdir->files) ;
}



/**************************************************************
 * Display structures
 **************************************************************/




string filesystem_xxdp_c::date_text(struct tm t) {
    string mon[] = { "JAN", "FEB", "MAR", "APR", "MAY", "JUN", "JUL", "AUG", "SEP", "OCT", "NOV",
                     "DEC"
                   };
    char buff[80];
    sprintf(buff, "%2d-%3s-%02d", t.tm_mday, mon[t.tm_mon].c_str(), t.tm_year);
    return string(buff);
}
// output like XXDP2.5
// ENTRY# BASENAME.EXT     DATE		  LENGTH  START   VERSION
//
//	   1  XXDPSM.SYS	   1-MAR-89 		29	  000050   E.0
//	   2  XXDPXM.SYS	   1-MAR-89 		39	  000105
// output Small monitor:
// ENTRY# FILNAM.EXT        DATE          LENGTH  START   VERSION


string filesystem_xxdp_c::directory_text_line(int fileidx)
{
    char buff[80];
    file_xxdp_c *f;
    if (fileidx < 0)
        // return "ENTRY# FILNAM.EXT		 DATE		   LENGTH  START   VERSION";
        return "ENTRY# FILNAM.EXT        DATE          LENGTH  START   VERSION";
    f = file_get(fileidx);
    assert(f->block_nr_list.size() > 0);
    sprintf(buff, "%5d  %6s.%-3s%15s%11d    %06o", fileidx + 1, f->basename.c_str(), f->ext.c_str(),
            date_text(f->modification_time).c_str(), (unsigned)f->block_nr_list.size(), f->block_nr_list[0]);
    return string(buff) ;
}

void filesystem_xxdp_c::print_directory(FILE *stream)
{
    // print a DIR like XXDP

    // header
    fprintf(stream, "%s\n", directory_text_line(-1).c_str());
    fprintf(stream, "\n");
    // files
    for (unsigned i = 0; i < file_count(); i++)
        if (!file_get(i)->internal)
            fprintf(stream, "%s\n", directory_text_line(i).c_str());

    fprintf(stream, "\n");
    fprintf(stream, "FREE BLOCKS: %d\n", blockcount - bitmap.used_block_count());
}


// usage of each block
void filesystem_xxdp_c::print_diag(FILE *stream)
{
    unsigned  n;
    char line[256];
    char buff[256];
    bool  used;
    fprintf(stream, "Filesystem has %d blocks, usage:\n", blockcount);
    for (xxdp_blocknr_t block_nr = 0; block_nr < blockcount; block_nr++) {
        line[0] = 0;
        if (block_nr == 0) {
            sprintf(buff, " BOOTBLOCK \"%s.%s\"", XXDP_BOOTBLOCK_BASENAME, XXDP_BOOTBLOCK_EXT);
            strcat(line, buff);
        }

        // monitor_core_image
        n = monitor_max_block_count;
        int blkoffset = block_nr - monitor_start_block_nr;
        if (blkoffset >= 0 && blkoffset < (int)n) {
            sprintf(buff, " MONITOR \"%s.%s\" - %d/%d", XXDP_MONITOR_BASENAME, XXDP_MONITOR_EXT,
                    blkoffset, n);
            strcat(line, buff);
        }

        // search mfd1, mfd2
        n = mfd_block_list.size() ; // blocks in linked list
        for (unsigned i = 0; i < n; i++) {
            xxdp_linked_block_c *block = &mfd_block_list[i] ;
            if (block_nr == block->get_block_nr() ) {
                sprintf(buff, " MFD - %d/%d", i, n);
                strcat(line, buff);
            }
        }
        // search ufd
        n = ufd_block_list.size();
        for (unsigned i = 0; i < n; i++) {
            xxdp_linked_block_c *block = &ufd_block_list[i] ;
            if (block_nr == block->get_block_nr() ) {
                sprintf(buff, " UFD - %d/%d", i, n);
                strcat(line, buff);
            }
        }
        // search bitmap
        n = bitmap.block_list.size();
        for (unsigned i = 0; i < n; i++) {
            xxdp_linked_block_c *block = &bitmap.block_list[i] ;
            if (block_nr == block->get_block_nr() ) {
                sprintf(buff, " BITMAP - %d/%d", i, n);
                strcat(line, buff);
            }
        }
        // search file
        for (unsigned file_idx = 0; file_idx < file_count(); file_idx++) {
            file_xxdp_c *f = file_get(file_idx);
            n = f->block_nr_list.size();
            for (unsigned j = 0; j < n; j++)
                if (f->block_nr_list[j] == block_nr) {
                    sprintf(buff, " file #%d: \"%s.%s\" - %d/%d", file_idx, f->basename.c_str(), f->ext.c_str(), j, n);
                    strcat(line, buff);
                }
        }
        // block marked in bitmap?
        used = bitmap.used[block_nr];
        if ((!used && line[0]) || (used && !line[0])) {
            sprintf(buff, " Bitmap mismatch, marked as %s!", used ? "USED" : "NOT USED");
            strcat(line, buff);
        }
        if (line[0]) {
            int offset = (blocknr2offset(block_nr) - blocknr2offset(0));
            fprintf(stream, "%5d @ 0x%06x = %#08o:	%s\n", block_nr, offset, offset, line);
        }
    }
    n = bitmap.used_block_count();
    fprintf(stream, "Blocks marked as \"used\" in bitmap: %d. Free: %d - %d = %d.\n", n,
            blockcount, n, blockcount - n);
}



} // namespace


