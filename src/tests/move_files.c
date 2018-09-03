/**
 * To run this test:
 *
 * 1. Build darktable per the standard instructions (https://www.darktable.org/install/)
 * 2. cd into the `build` directory
 * 3. Compile the test using the following: gcc -I../src -I../src/external -I/usr/include/gtk-3.0 -I/usr/include/cairo -I/usr/include/pango-1.0 -I/usr/include/gdk-pixbuf-2.0 -I/usr/include/atk-1.0 -I/usr/include/json-glib-1.0 $(pkg-config --cflags glib-2.0) -Lsrc -o test ../src/tests/move_files.c ../src/common/move_files.c $(pkg-config --libs glib-2.0) -ldarktable -lsqlite3
 * 4. Run the test like so: `./test`
 *
 * At the time of this writing, the tests don't do anything meaningful. They simply call the move_image function so that debugging can proceed.
 */

#include "common/move_files.h"
#include <stdio.h>
#define DT_TEST_PATTERN "/tmp/dt-test/$(EXIF_YEAR).$(EXIF_MONTH).$(EXIF_DAY)/$(TITLE)/$(CREATOR)-$(id)"








// Helper functions

/**
 * Inserts the given image and returns the image id
 */
int insert_test_image()
{
    return 1;
}






// Test Functions

/**
 * Tests moving/renaming a single file with no duplicates
 */
int test_single_normal_move()
{
    int id = insert_test_image();
    int result = dt_move_image(id, DT_TEST_PATTERN, 1, 1);
    return result;
}

/**
 * Tests moving/renaming multiple files, none of which have duplicates
 */
int test_multi_normal_move()
{
    return 0;
}

/**
 * Tests moving/renaming a file that has an unselected duplicate
 */
int test_partial_group_move()
{
    return 0;
}

/**
 * Tests moving/renaming a fully-selected group of duplicates
 */
int test_full_group_move()
{
    return 0;
}

/**
 * Tests renaming files in the same directory with conflicts
 */
int test_rename_in_same_dir_with_conflicts()
{
    return 0;
}

/**
 * Tests moving files to different directory with conflicts
 */
int test_move_to_foreign_with_conflicts()
{
    return 0;
}








// Main

int main()
{
    unsigned int result = 0;

    result += test_single_normal_move();
    result += test_multi_normal_move();
    result += test_partial_group_move();
    result += test_full_group_move();
    //result += test_rename_in_same_dir_with_conflicts();
    //result += test_move_to_foreign_with_conflicts();

    if (result == 0) {
        printf("\nAll tests passed!\n");
    } else {
        printf("\nSome tests failed :(.\n");
    }
    return result;
}


