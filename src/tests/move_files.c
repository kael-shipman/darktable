/**
 * To run this test:
 *
 * 1. Build darktable per the standard instructions (https://www.darktable.org/install/)
 * 2. cd into the `build` directory
 * 3. Compile the test using the following: gcc -I../src -I../src/external -I/usr/include/gtk-3.0 -I/usr/include/cairo -I/usr/include/pango-1.0 -I/usr/include/gdk-pixbuf-2.0 -I/usr/include/atk-1.0 -I/usr/include/json-glib-1.0 $(pkg-config --cflags glib-2.0) -Lsrc -g -o test ../src/tests/move_files.c ../src/common/move_files.c $(pkg-config --libs glib-2.0) -ldarktable -lsqlite3
 * 4. Run the test like so: `./test`
 *
 * At the time of this writing, the tests don't do anything meaningful. They simply call the move_image function so that debugging can proceed.
 */

#include "common/darktable.h"
#include "common/move_files.h"
#include "common/database.h"
#include <stdio.h>
#include <glib.h>
#include <fcntl.h>
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
 * Tests getting target path under various conditions
 */
int test_dt_image_get_targ_path()
{
    int err = 0;
    int test_fd = -1;
    int failures = 0;
    char* targ_path = NULL;
    char* expected_path = (char*)calloc(sizeof(char), 256);
    char* assertion = (char*)calloc(sizeof(char), 256);

    printf("TEST dt_image_get_targ_path\n\n");

    // Create a test output dir and add a file (to test overwrite directive)
    char* output_dir = "/tmp/dt-test/2018/test";
    if (g_mkdir_with_parents(output_dir, 0755))
    {
        fprintf(stderr, "[test_dt_image_get_targ_path]: could not create directory: `%s'!\n", output_dir);
        err = 10;
    }
    if (err == 0 && g_access(output_dir, W_OK | X_OK) != 0)
    {
        fprintf(stderr, "[test_dt_image_get_targ_path]: directory `%s' exists, but is not writeable!\n", output_dir);
        err = 30;
    }
    if (err == 0) {
        char* test_file = "/darktable-test.jpg";
        int full_length = strlen(output_dir) + strlen(test_file)+1;
        char* full_test_file_path = (char*)calloc(sizeof(char), full_length);
        snprintf(full_test_file_path, full_length, "%s%s", output_dir, test_file);
        if (!g_file_test(full_test_file_path, G_FILE_TEST_EXISTS)) {
            test_fd = g_open(full_test_file_path, O_RDWR | O_CREAT, S_IRUSR | S_IRGRP | S_IROTH);
            if (test_fd < 0) {
                fprintf(stderr, "[test_dt_image_get_targ_path]: couldn't create test file '%s'\n", full_test_file_path);
                err = 40;
            }
        }
        free(full_test_file_path);
    }

    if (err > 0) {
        exit(err);
    }

    dt_variables_params_t *params = NULL;
    dt_variables_params_init(&params);
    params->imgid = 1;
    params->jobcode = "test";
    params->sequence = 1;
    const char* origFilename = params->filename;



    // ASSERTIONS---------------------------------
    // TODO: Insert test image in database with known values
    // For now, it's expected that EXIF_YEAR on img 1 is 2018 and TITLE is 'test'

    // 1. Substitute normal variables in directory, overwriting existing
    params->filename = "/tmp/dt-test/source/darktable-test.jpg";
    assertion = "Substitute normal variables in directory, overwriting existing";
    targ_path = dt_image_get_targ_path(params, "/tmp/dt-test/$(EXIF_YEAR)/$(TITLE)/", 1);
    expected_path = "/tmp/dt-test/2018/test/darktable-test.jpg";
    if (strcmp(targ_path, expected_path) == 0) {
        printf(" * PASS: %s\n", assertion);
    } else {
        printf(" * FAIL: %s: targ_path is '%s'; expected '%s'\n", assertion, targ_path, expected_path);
    }

    // 2. Substitute normal variables in directory, NOT overwriting existing
    params->filename = "/tmp/dt-test/source/darktable-test.jpg";
    assertion = "Substitute normal variables in directory, NOT overwriting existing";
    targ_path = dt_image_get_targ_path(params, "/tmp/dt-test/$(EXIF_YEAR)/$(TITLE)/", 0);
    expected_path = "/tmp/dt-test/2018/test/darktable-test_01.jpg";
    if (strcmp(targ_path, expected_path) == 0) {
        printf(" * PASS: %s\n", assertion);
    } else {
        printf(" * FAIL: %s: targ_path is '%s'; expected '%s'\n", assertion, targ_path, expected_path);
    }

    // 3. Substitute normal variables in fully-specified file path, overwriting existing
    params->filename = "/tmp/dt-test/source/some-file.jpg";
    assertion = "Substitute normal variables in full file path, overwriting existing";
    targ_path = dt_image_get_targ_path(params, "/tmp/dt-test/$(EXIF_YEAR)/$(TITLE)/$(CREATOR)-test", 1);
    expected_path = "/tmp/dt-test/2018/test/darktable-test.jpg";
    if (strcmp(targ_path, expected_path) == 0) {
        printf(" * PASS: %s\n", assertion);
    } else {
        printf(" * FAIL: %s: targ_path is '%s'; expected '%s'\n", assertion, targ_path, expected_path);
    }

    // 4. Substitute normal variables in fully-specified file path, NOT ovewriting existing
    params->filename = "/tmp/dt-test/source/some-file.jpg";
    assertion = "Substitute normal variables in full file path, NOT overwriting existing";
    targ_path = dt_image_get_targ_path(params, "/tmp/dt-test/$(EXIF_YEAR)/$(TITLE)/$(CREATOR)-test", 0);
    expected_path = "/tmp/dt-test/2018/test/darktable-test_01.jpg";
    if (strcmp(targ_path, expected_path) == 0) {
        printf(" * PASS: %s\n", assertion);
    } else {
        printf(" * FAIL: %s: targ_path is '%s'; expected '%s'\n", assertion, targ_path, expected_path);
    }

    // END ASSERTIONS---------------------------------




    // TODO: Handle error
    if (test_fd != -1) {
        GError *ferrs;
        g_close(test_fd, &ferrs);
    }

    // Free mem
    params->filename = origFilename;
    free(params);
    free(targ_path);

    return 0;
}

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

int main(int argc, char *argv[])
{
    if (dt_init(argc, argv, FALSE, TRUE, NULL)) {
        fprintf(stderr, "Couldn't initialize darktable.\n");
        exit(1);
    }

    unsigned int result = 0;

    result += test_dt_image_get_targ_path();
    //result += test_single_normal_move();
    //result += test_multi_normal_move();
    //result += test_partial_group_move();
    //result += test_full_group_move();
    //result += test_rename_in_same_dir_with_conflicts();
    //result += test_move_to_foreign_with_conflicts();

    if (result == 0) {
        printf("\nAll tests passed!\n");
    } else {
        printf("\nSome tests failed :(.\n");
    }

    return result;
}


