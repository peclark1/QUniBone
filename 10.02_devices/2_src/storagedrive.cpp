/* storagedrive.cpp: disk or tape drive, with an image file as storage medium.

 Copyright (c) 2018, Joerg Hoppe
 j_hoppe@t-online.de, www.retrocmp.com

 Permission is hereby granted, free of charge, to any person obtaining a
 copy of this software and associated documentation files (the "Software"),
 to deal in the Software without restriction, including without limitation
 the rights to use, copy, modify, merge, publish, distribute, sublicense,
 and/or sell copies of the Software, and to permit persons to whom the
 Software is furnished to do so, subject to the following conditions:

 The above copyright notice and this permission notice shall be included in
 all copies or substantial portions of the Software.

 THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 JOERG HOPPE BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
 IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.


 may-2019		JD		file_size()
 12-nov-2018  JH      entered beta phase

 A storagedrive is a disk or tape drive, with an image file as storage medium.
 a couple of these are connected to a single "storagecontroler"
 supports the "attach" command.

 The image maybe an plain binary file, or a shared host directory holding an unpacked DEC filesystem.
 */
#include <assert.h>

#include <fstream>
#include <ios>
using namespace std;

#include "logger.hpp"
#include "storagedrive.hpp"

storagedrive_c::storagedrive_c(storagecontroller_c *controller) :
    device_c() {
    this->controller = controller;
    /*
     // parameters for all drices
     param_add(&unitno) ;
     param_add(&capacity) ;
     param_add(&image_filepath) ;
     */
     
	image = new storageimage_binfile_c() ;
	// or pure "shared" directoy, or syncronizing share<->binary image
}
	
storagedrive_c::~storagedrive_c(){
	delete image ;
}

// implements params, so must handle "change"
bool storagedrive_c::on_param_changed(parameter_c *param) {
    // no own "enable" logic
    return device_c::on_param_changed(param);
}




// fill buffer with test data to be placed at "file_offset"
void storagedrive_selftest_c::block_buffer_fill(unsigned block_number) {
    assert((block_size % 4) == 0); // whole uint32
    for (unsigned i = 0; i < block_size / 4; i++) {
        // i counts dwords in buffer
        // pattern: global incrementing uint32
        uint32_t pattern = i + (block_number * block_size / 4);
        ((uint32_t*) block_buffer)[i] = pattern;
    }
}

// verify pattern generated by fillbuff
void storagedrive_selftest_c::block_buffer_check(unsigned block_number) {
    assert((block_size % 4) == 0);	// whole uint32
    for (unsigned i = 0; i < block_size / 4; i++) {
        // i counts dwords in buffer
        // pattern: global incrementing uint32
        uint32_t pattern_expected = i + (block_number * block_size / 4);
        uint32_t pattern_found = ((uint32_t*) block_buffer)[i];
        if (pattern_expected != pattern_found) {
            printf(
                "ERROR storage_drive selftest: Block %d, dword %d: expected 0x%x, found 0x%x\n",
                block_number, i, pattern_expected, pattern_found);
            exit(1);
        }
    }
}

// self test of random access file interface
// test file has 'block_count' blocks with 'block_size' bytes capacity each.
void storagedrive_selftest_c::test() {
    unsigned i;
    bool *block_touched = (bool *) malloc(block_count * sizeof(bool)); // dyn array
    int blocks_to_touch;

    /*** fill all blocks with random accesses, until all blcoks touched ***/
    image->open(imagefname, true);

    for (i = 0; i < block_count; i++)
        block_touched[i] = false;

    blocks_to_touch = block_count;
    while (blocks_to_touch > 0) {
        unsigned block_number = random() % block_count;
        block_buffer_fill(block_number);
        image->write(block_buffer, /*position*/block_size * block_number, block_size);
        if (!block_touched[block_number]) { // mark
            block_touched[block_number] = true;
            blocks_to_touch--;
        }
    }
    image->close();

    /*** verify all blocks with random accesses, until all blcoks touched ***/
    image->open(imagefname, true);

    for (i = 0; i < block_count; i++)
        block_touched[i] = false;

    blocks_to_touch = block_count;
    while (blocks_to_touch > 0) {
        unsigned block_number = random() % block_count;
        image->read(block_buffer, /*position*/block_size * block_number, block_size);
        block_buffer_check(block_number);
        if (!block_touched[block_number]) { // mark
            block_touched[block_number] = true;
            blocks_to_touch--;
        }
    }

    image->close();

    free(block_touched);
}

