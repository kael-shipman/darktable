/**
 * To run this test:
 *
 * 1. Build darktable per the standard instructions (https://www.darktable.org/install/)
 * 2. cd into the `build` directory
 * 3. Compile the test using the following: gcc -I../src -I../src/external -I/usr/include/gtk-3.0 -I/usr/include/cairo -I/usr/include/pango-1.0 -I/usr/include/gdk-pixbuf-2.0 -I/usr/include/atk-1.0 -I/usr/include/json-glib-1.0 $(pkg-config --cflags glib-2.0 gio-2.0) -Lsrc -g -o test ../src/tests/image.c $(pkg-config --libs glib-2.0 gio-2.0) -ldarktable -lsqlite3
 * 4. Run the test like so: `./test`
 *
 * At the time of this writing, the tests don't do anything meaningful. They simply call the move_image function so that debugging can proceed.
 *
 * **NOTE:** It is now necessary to create a 'fixtures' directory at repo root with a 'pure' directory in it that contains
 * a valid library.db file with at least one test film roll. I created this for myself by following these steps:
 *
 * 1. `mkdir -p fixtures/test`
 * 2. `cp -R ~/Pictures/2010.12.24 fixtures/test/`
 * 2. `cp -R fixtures/test fixtures/pure`
 * 3. `mv ~/.config/darktable ~/.config/darktable.bak`
 * 4. Open darktable and import the `fixtures/test/2010.12.24` directory
 * 5. Set Title of all photos to `test` and Creator to `darktable`
 * 6. Close darktable
 * 7. `cp ~/.config/darktable/library.db fixtures/pure/`
 *
 * Now every time the tests are run, the `fixtures/test` directory is deleted and the `pure` directory (along with its
 * library.db file) is copied to `fixtures/test`, guaranteeing fresh tests every time.
 */

#include "common/darktable.h"
#include "common/image.h"
#include "common/database.h"
#include <stdio.h>
#include <glib.h>
#include <gio/gio.h>
#include <fcntl.h>
#include <unistd.h>
#define DT_TEST_PATTERN "/tmp/dt-test/$(EXIF_YEAR).$(EXIF_MONTH).$(EXIF_DAY)/$(TITLE)/$(CREATOR)-$(ID)"





// Test Functions

/**
 * Tests getting target path under various conditions
 *
 * NOTE: At the time of this writing, for this to work, you need to have a test DT instance set up where image #1 has
 * createor 'darktable', title 'test', and EXIF year '2010'. Eventually I'd like to create this fixture right here
 * in the test itself, but it was too much to figure out right now.
 */
int test_dt_image_get_path_for_pattern()
{
    int err = 0;
    int test_fd = -1;
    int failures = 0;
    char* targ_path = NULL;
    char* expected_path = (char*)calloc(sizeof(char), 256);
    char* assertion;

    printf("TEST dt_image_get_path_for_pattern\n\n");

    // Create a test output dir and add a file (to test overwrite directive)
    char* output_dir = "/tmp/dt-test/2010/test";
    if (g_mkdir_with_parents(output_dir, 0755))
    {
        fprintf(stderr, "[test_dt_image_get_path_for_pattern]: could not create directory: `%s'!\n", output_dir);
        err = 10;
    }
    if (err == 0 && g_access(output_dir, W_OK | X_OK) != 0)
    {
        fprintf(stderr, "[test_dt_image_get_path_for_pattern]: directory `%s' exists, but is not writeable!\n", output_dir);
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
                fprintf(stderr, "[test_dt_image_get_path_for_pattern]: couldn't create test file '%s'\n", full_test_file_path);
                err = 40;
            }
        }
        free(full_test_file_path);
    }

    // If there were any errors, setting up the test, just exit.
    if (err > 0) {
        exit(err);
    }


    // Now time to do the assertions

    // Set up. For now, we're hard-coding image id 1 and setting it up manually, but this should be improved....
    dt_variables_params_t *params = NULL;
    dt_variables_params_init(&params);
    params->imgid = 1;
    params->jobcode = "test";
    params->sequence = 1;
    const char* origFilename = params->filename;



    // ASSERTIONS---------------------------------
    // TODO: Insert test image in database with known values (see comment at top of function)

    // 1. Substitute normal variables in directory, overwriting existing
    params->filename = "/tmp/dt-test/source/darktable-test.jpg";
    assertion = "Substitute normal variables in directory, overwriting existing";
    targ_path = dt_image_get_path_for_pattern(params, "/tmp/dt-test/$(EXIF_YEAR)/$(TITLE)/", 1);
    expected_path = "/tmp/dt-test/2010/test/darktable-test.jpg";
    if (strcmp(targ_path, expected_path) == 0) {
        printf(" * PASS: %s\n", assertion);
    } else {
        printf(" * FAIL: %s: targ_path is '%s'; expected '%s'\n", assertion, targ_path, expected_path);
    }

    // 2. Substitute normal variables in directory, NOT overwriting existing
    params->filename = "/tmp/dt-test/source/darktable-test.jpg";
    assertion = "Substitute normal variables in directory, NOT overwriting existing";
    targ_path = dt_image_get_path_for_pattern(params, "/tmp/dt-test/$(EXIF_YEAR)/$(TITLE)/", 0);
    expected_path = "/tmp/dt-test/2010/test/darktable-test_01.jpg";
    if (strcmp(targ_path, expected_path) == 0) {
        printf(" * PASS: %s\n", assertion);
    } else {
        printf(" * FAIL: %s: targ_path is '%s'; expected '%s'\n", assertion, targ_path, expected_path);
    }

    // 3. Substitute normal variables in fully-specified file path, overwriting existing
    params->filename = "/tmp/dt-test/source/some-file.jpg";
    assertion = "Substitute normal variables in full file path, overwriting existing";
    targ_path = dt_image_get_path_for_pattern(params, "/tmp/dt-test/$(EXIF_YEAR)/$(TITLE)/$(CREATOR)-test", 1);
    expected_path = "/tmp/dt-test/2010/test/darktable-test.jpg";
    if (strcmp(targ_path, expected_path) == 0) {
        printf(" * PASS: %s\n", assertion);
    } else {
        printf(" * FAIL: %s: targ_path is '%s'; expected '%s'\n", assertion, targ_path, expected_path);
    }

    // 4. Substitute normal variables in fully-specified file path, NOT ovewriting existing
    params->filename = "/tmp/dt-test/source/some-file.jpg";
    assertion = "Substitute normal variables in full file path, NOT overwriting existing";
    targ_path = dt_image_get_path_for_pattern(params, "/tmp/dt-test/$(EXIF_YEAR)/$(TITLE)/$(CREATOR)-test", 0);
    expected_path = "/tmp/dt-test/2010/test/darktable-test_01.jpg";
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
    int result = dt_image_move_with_pattern(1, DT_TEST_PATTERN, 1, 1);
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






// Setup functions

static gboolean rm_rf(GFile *file);
static gboolean cp_r(GFile *file, char* targ);

/**
 * 1. Find the fixtures directory and make sure we have access
 * 2. Delete 'fixtures/test', if exists.
 * 3. Copy 'fixtures/pure' to 'fixtures/test' for fresh fixtures
 * 4. Set 'db_path' to 'fixtures/test/library' (relative to CWD)
 */
int set_up_fixtures(char **db_path)
{
    // Get the current working directory
    char cwd[PATH_MAX];
    if (getcwd(cwd, sizeof(cwd)) == NULL) {
        printf("[set_up_fixtures]: ERROR: Couldn't get current working directory.\n");
        return 10;
    }

    // Try a couple of options to see if we can find the fixtures directory
    char *fixtures_path;

    // First assume we're in 'build' and try '../fixtures'
    fixtures_path = g_build_path(G_DIR_SEPARATOR_S, cwd, "..", "fixtures", NULL);
    if (g_access(fixtures_path, F_OK) != 0) {
        // Then try just './fixtures'
        g_free(fixtures_path);
        fixtures_path = g_build_path(G_DIR_SEPARATOR_S, cwd, "fixtures", NULL);
        if (g_access(fixtures_path, F_OK) != 0) {
            // Give up
            g_free(fixtures_path);
            printf(
                    "[set_up_fixtures]: ERROR: Couldn't find fixtures path. You should run these tests from\n"
                    "                   either repo root or ./build\n"
            );
            return 20;
        }
    }

    // Now make sure we've got write access
    if (g_access(fixtures_path, R_OK | W_OK) != 0) {
        printf(
                    "[set_up_fixtures]: ERROR: Access to '%s' is denied.\n"
                    "                   Please update your permissions.\n",
                    fixtures_path
        );
        return 30;
    }

    char *pure_path = g_build_path(G_DIR_SEPARATOR_S, fixtures_path, "pure", NULL);
    char *test_path = g_build_path(G_DIR_SEPARATOR_S, fixtures_path, "test", NULL);

    // Delete 'fixtures/test', if exists
    if (g_access(test_path, F_OK) == 0) {
        GFile *test_path_gfile = g_file_new_for_path(test_path);
        if (rm_rf(test_path_gfile) == FALSE) {
            g_free(fixtures_path);
            g_free(pure_path);
            g_free(test_path);
            printf("[set_up_fixtures]: ERROR: Couldn't delete existing test path '%s'.\n", test_path);
            return 40;
        }
        g_object_unref(test_path_gfile);
    }

    // Now copy 'fixtures/pure' to 'fixtures/test'
    GFile *pure_path_gfile = g_file_new_for_path(pure_path);
    if (cp_r(pure_path_gfile, test_path) == FALSE) {
        g_free(fixtures_path);
        g_free(pure_path);
        g_free(test_path);
        printf(
                "[set_up_fixtures]: ERROR: Couldn't copy '%s'\n"
                "                   to '%s'.\n",
                pure_path,
                test_path
        );
        return 50;
    }
    g_object_unref(pure_path_gfile);

    // Set db_path
    *db_path = g_build_path(G_DIR_SEPARATOR_S, test_path, "library.db", NULL);

    // Free memory
    g_free(fixtures_path);
    g_free(pure_path);

    // Yay!
    return 0;
}

/**
 * Recursive remove directory
 * (from https://stackoverflow.com/questions/43377924/is-there-a-similar-function-to-boostfilesystemremove-allpath-in-glib-or-li)
 */
static gboolean rm_rf(GFile *file)
{
    g_autoptr(GFileEnumerator) enumerator = NULL;

    enumerator = g_file_enumerate_children (file, G_FILE_ATTRIBUTE_STANDARD_NAME,
            G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
            (GCancellable*)NULL, NULL);

    while (enumerator != NULL) {
        GFile *child;

        if (!g_file_enumerator_iterate (enumerator, NULL, &child, (GCancellable*)NULL, (GError**)NULL))
            return FALSE;
        if (child == NULL)
            break;
        if (!rm_rf (child))
            return FALSE;
    }

    return g_file_delete (file, (GCancellable*)NULL, (GError**)NULL);
}

/**
 * Recursive copy
 */
static gboolean cp_r(GFile *file, char* targ)
{
    if (g_file_test(g_file_get_path(file), G_FILE_TEST_IS_DIR) == TRUE) {
        // Make the target directory
        // TODO: transfer permissions, etc.
        g_mkdir_with_parents(targ, 0755);

        // Iterate through children and copy them over
        g_autoptr(GFileEnumerator) enumerator = NULL;
        enumerator = g_file_enumerate_children (file, G_FILE_ATTRIBUTE_STANDARD_NAME,
                G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                (GCancellable*)NULL, NULL);

        while (enumerator != NULL) {
            GFile *child;

            if (!g_file_enumerator_iterate (enumerator, NULL, &child, (GCancellable*)NULL, (GError**)NULL))
                return FALSE;
            if (child == NULL)
                break;

            char *child_targ = g_build_path(G_DIR_SEPARATOR_S, targ, g_file_get_basename(child), NULL);
            if (!cp_r (child, child_targ))
                return FALSE;
        }
        return TRUE;
    } else {
        GFile *targ_gfile = g_file_new_for_path(targ);
        return g_file_copy(
            file,
            targ_gfile,
            G_FILE_COPY_OVERWRITE | G_FILE_COPY_NOFOLLOW_SYMLINKS | G_FILE_COPY_ALL_METADATA,
            (GCancellable*)NULL,
            (GFileProgressCallback)NULL,
            (gpointer)NULL,
            (GError**)NULL
        );
    }
}






// Main

/**
 * TODO: Figure out how to set up fixtures that work on other devs' machines
 *
 * Currently, the fixtures library.db contains paths that are specific to my personal computer.
 */
int main(int argc, char *argv[])
{
    char** our_argv = malloc(argc * sizeof(*our_argv));
    memmove(our_argv, argv, sizeof(*our_argv) * argc);

    // If we haven't explicitly passed in a different library path, set up fixtures and use them
    int use_fixtures = 1;
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--library") == 0) {
            use_fixtures = 0;
            break;
        }
    }

    if (use_fixtures == 1) {
        char *db_path;
        if (set_up_fixtures(&db_path) > 0) {
            printf("[main]: ERROR: Couldn't set up fixtures for tests. Exiting.\n");
            return 5;
        }
        our_argv = realloc(our_argv, (argc + 2) * sizeof(*our_argv));

        our_argv[argc++] = "--library";

        our_argv[argc] = (char*)malloc(sizeof(char)*PATH_MAX);
        g_strlcpy(our_argv[argc], db_path, sizeof(char)*PATH_MAX);
        argc++;
    }

    if (dt_init(argc, our_argv, FALSE, TRUE, NULL)) {
        fprintf(stderr, "Couldn't initialize darktable.\n");
        exit(1);
    }

    unsigned int result = 0;

    result += test_dt_image_get_path_for_pattern();
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

    dt_database_destroy(darktable.db);

    return result;
}


