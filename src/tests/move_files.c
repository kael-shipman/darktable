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
    int result = dt_move_image(id, DT_TEST_PATTERN);
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


