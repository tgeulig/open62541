#include <stdlib.h>
#include "check.h"
#include "ua_server.h"
#include "testing_networklayers.h"
#include "logger_stdout.h"

#include <stdio.h>

UA_Boolean running = 1;
UA_Int32 read_count = 0;
UA_Int32 max_reads = 5;
char *filename;

static void writeCallback(void *handle, UA_ByteStringArray buf) {
}

static void readCallback(void) {
    read_count++;
    if(read_count >= max_reads)
        running = UA_FALSE;
}

static UA_StatusCode runServerWithFileInput(char *filename) {
	UA_Server *server = UA_Server_new();
    UA_Server_setLogger(server, Logger_Stdout_new());
    UA_Server_addNetworkLayer(server, ServerNetworkLayerFileInput_new(filename, readCallback,
                                                                      writeCallback, NULL));
    UA_StatusCode retval = UA_Server_run(server, 1, &running);
	UA_Server_delete(server);
    return retval;
}

START_TEST(readVariable) {
    UA_StatusCode ret = runServerWithFileInput(filename);
	ck_assert_int_eq(ret, UA_STATUSCODE_GOOD);
}
END_TEST

static Suite *testSuite_builtin(void) {
	Suite *s = suite_create("Test server with client messages stored in text files");

	TCase *tc_nano = tcase_create("nano profile");
	tcase_add_test(tc_nano, readVariable);
	suite_add_tcase(s, tc_nano);

	return s;
}

int main(int argc, char **argv) {
    if(argc < 2)
        return EXIT_FAILURE;
    filename = argv[1];
	int      number_failed = 0;
	Suite   *s;
	SRunner *sr;
	s  = testSuite_builtin();
	sr = srunner_create(s);
	srunner_set_fork_status(sr, CK_NOFORK);
	srunner_run_all(sr, CK_NORMAL);
	number_failed += srunner_ntests_failed(sr);
	srunner_free(sr);
	return (number_failed == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}
