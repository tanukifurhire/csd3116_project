/* gcc -o server server.c messages.c $(pkg-config --cflags --libs CycloneDDS) */

/* Dr Khoo Teck Ping
27 Aug 2026
1. DDS server
2. Supports max 5 clients
3. Start this server, then start client one by one
4. Each client can see itself and all other clients
5. Each client can move itself using the arrow keys
6. Collision detection among all players and the window is supported
7. Each player must supply a unique (case-insensitive) name, max 8 chars;
   duplicate names are rejected so the client can ask for another one */

/* mutex lock/unlock must be matched
watch out for break out of loop */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>

#include "dds/dds.h"
#include "messages.h"

// ### GLOBAL CONSTANTS ###
/* An array of one message (aka sample in dds terms) will be used. */
#define MAX_SAMPLES 1
#define MAX_PLAYERS 5

#define MAX_NAME_LEN 8

#define WIDTH  800
#define HEIGHT 600
#define SQ_WIDTH 50
// How many pixels is one step
#define STEP 1

// Player coord defined as NW corner of square
typedef struct
{
    bool b_active;
    int x;
    int y;
    char str_name[MAX_NAME_LEN + 1];
} Player;

// ### GLOBAL VARIABLES ###
int int_num_active_players = 0;

// Flag set by worker thread that a player just joined
// Must be reset to false after processing by main thread
bool b_player_join = false;

// Player ID is the array index plus 1
// -1 = No new players
// 0 = Invalid player ID (unable to admit new players)
// 1~MAX_PLAYERS = Valid player IDs
int int_new_player_id = -1;

// Name that goes with the join currently being processed (set by worker
// when it accepts a join request as unique, read by main when it assigns
// the ID, and read again by worker when it sends the response). Always
// accessed under 'mutex'.
char str_pending_name[MAX_NAME_LEN + 1] = {0};

// Table to track clients
// Init all members to 0
static Player arr_Players[MAX_PLAYERS] = {0};
pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;

// Check if the coords collide with other squares
bool detect_collision(Player a_arr_players[], int a_int_arr_len, 
						int a_int_id, int a_int_x, int a_int_y) 
{
	bool b_res = false;
	// Loop through the array
	for(int i = 0; i < a_int_arr_len; i++)
	{
		// Index + 1 = Player ID
		// If a player other than the player itself is active
		if(i != a_int_id -1 && a_arr_players[i].b_active == true)
		{
			// If they overlap horizontally
			if(a_int_x <= a_arr_players[i].x + SQ_WIDTH && 
				a_arr_players[i].x <= a_int_x + SQ_WIDTH)
			{
				// If they overlap vertically
				if(a_int_y <= a_arr_players[i].y + SQ_WIDTH && 
					a_arr_players[i].y <= a_int_y + SQ_WIDTH)
				{
					b_res = true;
					break;
				}
			}
		}
	}
	
	return b_res;
}

// Case-insensitive check for whether a_str_name is already in use by an
// active player. Caller must hold 'mutex'.
bool is_name_taken(const char *a_str_name)
{
	for (int i = 0; i < MAX_PLAYERS; i++)
	{
		if (arr_Players[i].b_active && strcasecmp(arr_Players[i].str_name, a_str_name) == 0)
		{
			return true;
		}
	}
	return false;
}

// ------------------------------------------------------------------
// DDS Security: creates a participant configured with the Authentication,
// Access Control, and Cryptographic plugins.
// ------------------------------------------------------------------
static char *file_uri(const char *path)
{
    size_t len = strlen(path) + 6; /* "file:" + path + NUL */
    char *uri = malloc(len);
    snprintf(uri, len, "file:%s", path);
    return uri;
}

dds_entity_t create_secure_participant(
    const char *identity_ca_path,
    const char *identity_cert_path,
    const char *private_key_path,
    const char *governance_path,
    const char *permissions_path)
{
    dds_qos_t *qos = dds_create_qos();

    char *identity_ca    = file_uri(identity_ca_path);
    char *identity_cert  = file_uri(identity_cert_path);
    char *private_key    = file_uri(private_key_path);
    char *permissions_ca = file_uri(identity_ca_path); /* same CA reused */
    char *governance     = file_uri(governance_path);
    char *permissions    = file_uri(permissions_path);

    /* --- Authentication plugin --- */
    dds_qset_prop(qos, "dds.sec.auth.identity_ca", identity_ca);
    dds_qset_prop(qos, "dds.sec.auth.identity_certificate", identity_cert);
    dds_qset_prop(qos, "dds.sec.auth.private_key", private_key);
    dds_qset_prop(qos, "dds.sec.auth.library.path", "dds_security_auth");
    dds_qset_prop(qos, "dds.sec.auth.library.init", "init_authentication");
    dds_qset_prop(qos, "dds.sec.auth.library.finalize", "finalize_authentication");

    /* --- Access Control plugin --- */
    dds_qset_prop(qos, "dds.sec.access.permissions_ca", permissions_ca);
    dds_qset_prop(qos, "dds.sec.access.governance", governance);
    dds_qset_prop(qos, "dds.sec.access.permissions", permissions);
    dds_qset_prop(qos, "dds.sec.access.library.path", "dds_security_ac");
    dds_qset_prop(qos, "dds.sec.access.library.init", "init_access_control");
    dds_qset_prop(qos, "dds.sec.access.library.finalize", "finalize_access_control");

    /* --- Cryptographic plugin --- */
    dds_qset_prop(qos, "dds.sec.crypto.library.path", "dds_security_crypto");
    dds_qset_prop(qos, "dds.sec.crypto.library.init", "init_crypto");
    dds_qset_prop(qos, "dds.sec.crypto.library.finalize", "finalize_crypto");

    dds_entity_t participant = dds_create_participant(DDS_DOMAIN_DEFAULT, qos, NULL);

    dds_delete_qos(qos);
    free(identity_ca);
    free(identity_cert);
    free(private_key);
    free(permissions_ca);
    free(governance);
    free(permissions);

    return participant;
}

void *worker(void *arg)
{
	dds_entity_t participant;
	dds_entity_t join_topic;
	dds_entity_t response_topic;
	dds_entity_t position_topic;
	dds_entity_t input_topic;
	dds_entity_t join_reader;
	dds_entity_t response_writer;
	dds_entity_t position_writer;
	dds_entity_t input_reader;
		
	//Pointer to unknown type
	void *samples_join[MAX_SAMPLES];	
	dds_sample_info_t infos_join[MAX_SAMPLES];
	void *samples_input[MAX_SAMPLES];	
	dds_sample_info_t infos_input[MAX_SAMPLES];
	
	dds_return_t rc;
	
	/* ----------------------------------------------------- */
    /* Create participant                                    */
    /* ----------------------------------------------------- */

    participant = create_secure_participant(
        "certs/ca.pem",
        "certs/server.pem",
        "certs/server.key",
        "certs/governance.p7s",
        "certs/permissions.p7s"
    );

    if (participant < 0)
    {
        printf("Failed to create participant: %s\n", dds_strretcode(-participant));
        return NULL;
    }

    /* ----------------------------------------------------- */
    /* Create topics                                          */
    /* ----------------------------------------------------- */

    join_topic = dds_create_topic
	(
        participant,
        &JoinRequest_desc,
        "JoinRequest",
        NULL,
        NULL
    );
	
	response_topic = dds_create_topic
	(
        participant,
        &JoinResponse_desc,
        "JoinResponse",
        NULL,
        NULL
    );
	
	position_topic = dds_create_topic
	(
        participant,
        &Position_desc,
        "Position",
        NULL,
        NULL
    );
	
	input_topic = dds_create_topic
	(
        participant,
        &Input_desc,
        "Input",
        NULL,
        NULL
    );
	/* ----------------------------------------------------- */
    /* Create readers                                         */
    /* ----------------------------------------------------- */

    join_reader = dds_create_reader(
        participant,
        join_topic,
        NULL,
        NULL
    );
	
	input_reader = dds_create_reader(
        participant,
        input_topic,
        NULL,
        NULL
    );
	
	 /* ----------------------------------------------------- */
    /* Create writers                                         */
    /* ----------------------------------------------------- */

    response_writer = dds_create_writer(
        participant,
        response_topic,
        NULL,
        NULL
    );
	
	position_writer = dds_create_writer(
        participant,
        position_topic,
        NULL,
        NULL
    );
	
	/* Initialize sample buffer, by pointing the void pointer within
	* the buffer array to a valid sample memory location. */
	samples_join[0] = JoinRequest__alloc();
	samples_input[0] = Input__alloc();
	
	// Create a Join Response
	JoinResponse response;
	
	// Create a Position
	Position position;
	
	// Record the time now
	uint64_t last_time = dds_time();	
	/* Poll until data has been read. */
	while (1)
	{
		// Process a join request
		rc = dds_take(join_reader, samples_join, infos_join, MAX_SAMPLES, MAX_SAMPLES);
		
		if (rc < 0)
			DDS_FATAL("dds_take: %s\n", dds_strretcode(-rc));

		// Check for valid join request
		if ((rc > 0) && (infos_join[0].valid_data))
		{
			printf("Join request received\n");

			JoinRequest *request = (JoinRequest *) samples_join[0];
			char str_req_name[MAX_NAME_LEN + 1];
			strncpy(str_req_name, request->str_name, MAX_NAME_LEN);
			str_req_name[MAX_NAME_LEN] = '\0';

			pthread_mutex_lock(&mutex);
			bool b_taken = is_name_taken(str_req_name);
			if (!b_taken)
			{
				// Stash the name for the main thread to store once it
				// assigns an ID, and for us to echo back in the response.
				strncpy(str_pending_name, str_req_name, MAX_NAME_LEN);
				str_pending_name[MAX_NAME_LEN] = '\0';
			}
			pthread_mutex_unlock(&mutex);

			if (b_taken)
			{
				// Reject right away - no need to involve the main thread.
				response.int_player_id = -1;
				strncpy(response.str_name, str_req_name, MAX_NAME_LEN);
				response.str_name[MAX_NAME_LEN] = '\0';

				rc = dds_write(response_writer, &response);
				if (rc != DDS_RETCODE_OK)
				{
					DDS_FATAL("dds_write: %s\n", dds_strretcode(-rc));
				}
				printf("Name '%s' already taken - join rejected\n", str_req_name);
			}
			else
			{
				// Signal the main thread to check if
				// number of active players reached
				pthread_mutex_lock(&mutex);
				b_player_join = true;
				pthread_mutex_unlock(&mutex);
			}
		}
		
		// If a new player ID has been assigned by the main thread
		pthread_mutex_lock(&mutex);
		int int_new_player_id_temp = int_new_player_id;
		/* printf("int_new_player_id_temp=%d\n", int_new_player_id_temp); */
		pthread_mutex_unlock(&mutex);
		if (int_new_player_id_temp != -1)
		{
			pthread_mutex_lock(&mutex);
			// Reset the player ID
			int_new_player_id = -1;
			// Grab the name this join was for, to echo it in the response
			char str_resp_name[MAX_NAME_LEN + 1];
			strncpy(str_resp_name, str_pending_name, MAX_NAME_LEN);
			str_resp_name[MAX_NAME_LEN] = '\0';
			pthread_mutex_unlock(&mutex);

			// The player ID can be 0, ie invalid
			response.int_player_id = int_new_player_id_temp;
			strncpy(response.str_name, str_resp_name, MAX_NAME_LEN);
			response.str_name[MAX_NAME_LEN] = '\0';
			
			rc = dds_write(response_writer, &response);
			if (rc != DDS_RETCODE_OK)
			{
				DDS_FATAL("dds_write: %s\n", dds_strretcode(-rc));
			}
			printf("Join response sent\n");
			/* Polling sleep. */
			dds_sleepfor (DDS_MSECS (20));
		}
		
		// Get the current time
		uint64_t curr_time = dds_time();
		// Get the time difference
		if (curr_time - last_time > DDS_SECS(0.1))
		{
			// Update the last time
			last_time = curr_time;
			
			// Loop through the players list
			for(int i = 0; i < MAX_PLAYERS; i++)
			{
				pthread_mutex_lock(&mutex);
				// If player is active
				if(arr_Players[i].b_active == true)
				{
					position.int_player_id = i+1;
					position.int_x = arr_Players[i].x;
					position.int_y = arr_Players[i].y;
					strncpy(position.str_name, arr_Players[i].str_name, MAX_NAME_LEN);
					position.str_name[MAX_NAME_LEN] = '\0';
					rc = dds_write(position_writer, &position);
					if (rc != DDS_RETCODE_OK)
					{
						DDS_FATAL("dds_write: %s\n", dds_strretcode(-rc));
					}
					printf("Position sent\n");
					/* Polling sleep. */
					dds_sleepfor (DDS_MSECS (20));
				}
				pthread_mutex_unlock(&mutex);
			}
		}
		
		// Listen for input
		rc = dds_take(input_reader, samples_input, infos_input, MAX_SAMPLES, MAX_SAMPLES);
		
		if (rc < 0)
			DDS_FATAL("dds_take: %s\n", dds_strretcode(-rc));

		// Check if we read some data and it is valid.
		if ((rc > 0) && (infos_input[0].valid_data))
		{
			printf("Input received\n");
			Input *input = (Input *) samples_input[0];
			// Get the input player ID
			int int_input_player_id = input->int_player_id;
			// Get the input direction
			char ch_input_dir = input->ch_direction;
			/* printf("%d=%c\n", int_input_player_id, ch_input_dir); */
			
			// Get the player's current coords
			pthread_mutex_lock(&mutex);
			// Index is player ID minus 1
			int int_x = arr_Players[int_input_player_id-1].x;
			int int_y = arr_Players[int_input_player_id-1].y;
			pthread_mutex_unlock(&mutex);
			
			/* printf("%d,%d\n", int_x, int_y); */
						
			switch (ch_input_dir)
			{
				case 'u':
					int_y = int_y - STEP;
					break;
				case 'd':
					int_y = int_y + STEP;
					break;
				case 'l':
					int_x = int_x - STEP;
					break;
				case 'r':
					int_x = int_x + STEP;
					break;
				
				default:
					break;
			}
			
			// printf("%d,%d\n", int_x, int_y);
			// printf("===========\n");
			
			// Check collision with window
			if(int_x >= 0 && int_x <= WIDTH-SQ_WIDTH)
			{
				if(int_y >= 0 && int_y <= HEIGHT-SQ_WIDTH)
				{
					// printf("%d,%d\n", int_x, int_y);
					// printf("===========\n");
					// Check collision with other players
					pthread_mutex_lock(&mutex);
					// Index is player ID minus 1
					bool b_res = detect_collision(arr_Players,
						MAX_PLAYERS, int_input_player_id, int_x, int_y);
					pthread_mutex_unlock(&mutex);
					// If no collision detected
					if(b_res == false)
					{
						//Update that player's position
						pthread_mutex_lock(&mutex);
						arr_Players[int_input_player_id-1].x = int_x;
						arr_Players[int_input_player_id-1].y = int_y;
						pthread_mutex_unlock(&mutex);
					}
				}
			}
			/* printf("input->int_player_id = %d\n", input->int_player_id);
			printf("input->dir = %c\n", input->ch_direction); */
		}
	}
}

int main(void)
{
	// Seed rand(), so that the random numbers are different for each run
	srand((unsigned int)time(NULL));
	
	pthread_t thread;

    if (pthread_create(&thread, NULL, worker, NULL) != 0)
    {
        perror("pthread_create");
        return 1;
    }
	
	printf("Waiting...\n");
	while (1)
	{
		// for(int i = 0; i < MAX_PLAYERS; i++)
		// {
			//printf("%d %d %d\n",i,arr_Players[i].x, arr_Players[i].y);
		// }
		//printf("============\n");
		/* dds_sleepfor (DDS_SECS (0.5)); */
					
		// If a join request has been received
		pthread_mutex_lock(&mutex);
		bool b_player_join_temp = b_player_join;
		pthread_mutex_unlock(&mutex);
		if (b_player_join_temp == true)
		{
			// printf("b_player_join_temp\n");
			// Reset the flag
			pthread_mutex_lock(&mutex);
			b_player_join = false;
			pthread_mutex_unlock(&mutex);
			
			// If max active players reached
			if (int_num_active_players == MAX_PLAYERS)
			{
				// Set invalid player ID
				pthread_mutex_lock(&mutex);
				int_new_player_id = 0;
				pthread_mutex_unlock(&mutex);
			}
			else
			{
				pthread_mutex_lock(&mutex);
				// Loop through the players list
				for(int i = 0; i < MAX_PLAYERS; i++)
				{					
					if(arr_Players[i].b_active == false)
					{
						// Set this index to active
						arr_Players[i].b_active = true;
						// Increment the number of active players
						int_num_active_players++;

						// Record the name this player joined with
						strncpy(arr_Players[i].str_name, str_pending_name, MAX_NAME_LEN);
						arr_Players[i].str_name[MAX_NAME_LEN] = '\0';
						
						// Calculate initial coords of the new player
						int int_rand_x = 0;
						int int_rand_y = 0;						
						while(1)
						{
							int_rand_x = rand()%(WIDTH-SQ_WIDTH+1);
							int_rand_y = rand()%(HEIGHT-SQ_WIDTH+1);
							// Function don't have to lock/unlock mutex
							bool b_res = detect_collision(arr_Players,
								MAX_PLAYERS, i+1, int_rand_x, int_rand_y);
							// If no collision detected
							if(b_res == false)
							{
								break;
							}
						}
						
						// Set the coords
						arr_Players[i].x = int_rand_x;
						arr_Players[i].y = int_rand_y;
						
						// Set player ID to be the index plus 1
						/* pthread_mutex_lock(&mutex); */
						int_new_player_id = i+1;
						/* printf("int_new_player_id=%d\n", int_new_player_id); */
						/* pthread_mutex_unlock(&mutex); */
						break;
					}
				}
				pthread_mutex_unlock(&mutex);
			}
		}		
	}
	
	return 0;
}
