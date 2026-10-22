// Test the server input checks by sending messages through DDS
// Rename the server main function so this file can provide its own main function
#define main baseline_main
#include "server.c"
#undef main
#include <assert.h>
int main(void) {
    pthread_t t;

    // Set Player 1 to active at a known position
    arr_Players[0] = (Player){true, 100, 100};

    // Start the same DDS worker used by the server
    assert(pthread_create(&t, NULL, worker, NULL) == 0);

    // Create another participant to send input to the server worker
    dds_entity_t p = dds_create_participant(DDS_DOMAIN_DEFAULT, NULL, NULL);
    assert(p > 0);
    dds_entity_t topic = dds_create_topic(p, &Input_desc, "Input", NULL, NULL);
    dds_entity_t w = dds_create_writer(p, topic, NULL, NULL);
    assert(w > 0);

    // Give the two participants time to discover each other
    dds_sleepfor(DDS_SECS(2));

    // Try invalid IDs then an invalid direction then an inactive player
    Input cases[] = {{0,'r'}, {-1,'r'}, {6,'r'}, {2147483647,'r'}, {1,'x'}, {2,'r'}};
    for (size_t i = 0; i < sizeof(cases)/sizeof(cases[0]); ++i) {
        assert(dds_write(w, &cases[i]) == DDS_RETCODE_OK);

        // Give the worker time to read this input before sending the next one
        dds_sleepfor(DDS_MSECS(100));
    }

    // Lock the player array before checking the position
    pthread_mutex_lock(&mutex);

    // Check that the rejected inputs did not move Player 1
    assert(arr_Players[0].x == 100 && arr_Players[0].y == 100);
    pthread_mutex_unlock(&mutex);

    // Send one allowed input for the active player
    Input valid = {1, 'r'};
    assert(dds_write(w, &valid) == DDS_RETCODE_OK);
    dds_sleepfor(DDS_MSECS(300));

    // Lock the player array before checking the position
    pthread_mutex_lock(&mutex);

    // Check that the allowed input moved Player 1 one step to the right
    assert(arr_Players[0].x == 101 && arr_Players[0].y == 100);
    pthread_mutex_unlock(&mutex);
    puts("PASS: six rejected inputs; valid input moves exactly one step");
    return 0;
}
