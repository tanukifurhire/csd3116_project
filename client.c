/* gcc client.c messages.c -o client $(pkg-config --cflags --libs glfw3) -lGL $(pkg-config --cflags --libs CycloneDDS) -pthread -lm*/

/* Dr Khoo Teck Ping
27 Aug 2026
1. DDS client
2. Supports max 5 clients
3. Start server, then start client one by one
4. Each client can see itself and all other clients
5. Each client can move itself using the arrow keys
6. Collision detection among all players and the window is supported
7. Each client must pick a unique (case-insensitive) player name, max 8 chars,
   before being admitted by the server */

#include <GLFW/glfw3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <pthread.h>
#include <unistd.h>
#include "dds/dds.h"
#include "messages.h"
#include "stb_easy_font.h"

#define STB_EASY_FONT_IMPLEMENTATION

#define MAX_SAMPLES 1
#define MAX_PLAYERS 5

#define MAX_NAME_LEN 8

#define WIDTH  800
#define HEIGHT 600
#define SQ_WIDTH 50


// Player coord defined as NW corner of square
typedef struct
{
    bool b_active;
    int x;
    int y;
    char str_name[MAX_NAME_LEN + 1];
} Player;

// ### GLOBAL VARIABLES ###
int int_player_id = -1;

// Table to track clients
// Init all members to 0
static Player arr_Players[MAX_PLAYERS] = {0};

// Direction - n means no direction
char ch_dir = 'n';
pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;

// This client's chosen name (set once before the worker thread starts,
// and possibly updated by the worker thread itself if the name is taken -
// never touched concurrently by two threads at once)
char str_my_name[MAX_NAME_LEN + 1] = {0};

// Which certs/clientN.pem + clientN.key this instance authenticates as.
// Set from argv[1] in main() before the worker thread starts (e.g. "1").
char str_client_id[4] = "1";

void draw_text(float x, float y, float scale, const char *text)
{
    char buffer[4096];

    int num_quads = stb_easy_font_print(
        0, 0,
        (char *)text,
        NULL,
        buffer,
        sizeof(buffer)
    );

    glPushMatrix();

    glTranslatef(x, y, 0.0f);
    glScalef(scale, scale, 1.0f);

    glEnableClientState(GL_VERTEX_ARRAY);

    glVertexPointer(2, GL_FLOAT, 16, buffer);
    glDrawArrays(GL_QUADS, 0, num_quads * 4);

    glDisableClientState(GL_VERTEX_ARRAY);

    glPopMatrix();
}

// Prompt on stdin for a name, truncate to MAX_NAME_LEN chars, strip newline.
// Falls back to "Player" if the user just hits enter.
void prompt_for_name(char *out_name)
{
    char str_input[64];

    if (fgets(str_input, sizeof(str_input), stdin) != NULL)
    {
        str_input[strcspn(str_input, "\n")] = '\0';
    }
    else
    {
        str_input[0] = '\0';
    }

    strncpy(out_name, str_input, MAX_NAME_LEN);
    out_name[MAX_NAME_LEN] = '\0';

    if (strlen(out_name) == 0)
    {
        strcpy(out_name, "Player");
    }
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
	dds_entity_t input_topic;
	dds_entity_t position_topic;
	dds_entity_t response_reader;
	dds_entity_t position_reader;
	dds_entity_t join_writer;
	dds_entity_t input_writer;
	
	//Pointer to unknown type
	void *samples_response[MAX_SAMPLES];
	dds_sample_info_t infos_response[MAX_SAMPLES];
	void *samples_position[MAX_SAMPLES];
	dds_sample_info_t infos_position[MAX_SAMPLES];
	dds_return_t rc;
	
	/* ----------------------------------------------------- */
    /* Create participant                                    */
    /* ----------------------------------------------------- */

    char str_cert_path[64];
    char str_key_path[64];
    snprintf(str_cert_path, sizeof(str_cert_path), "certs/client%s.pem", str_client_id);
    snprintf(str_key_path, sizeof(str_key_path), "certs/client%s.key", str_client_id);

    participant = create_secure_participant(
        "certs/ca.pem",
        str_cert_path,
        str_key_path,
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
	
	input_topic = dds_create_topic
	(
        participant,
        &Input_desc,
        "Input",
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
	
	/* ----------------------------------------------------- */
    /* Create writers                                         */
    /* ----------------------------------------------------- */

    join_writer = dds_create_writer(
        participant,
        join_topic,
        NULL,
        NULL
    );
	
	input_writer = dds_create_writer(
        participant,
        input_topic,
        NULL,
        NULL
    );
	
	/* ----------------------------------------------------- */
    /* Create readers                                         */
    /* ----------------------------------------------------- */

    response_reader = dds_create_reader(
        participant,
        response_topic,
        NULL,
        NULL
    );
	
	position_reader = dds_create_reader(
        participant,
        position_topic,
        NULL,
        NULL
    );
		
	JoinRequest request;
    request.b_dummy = true;
	strncpy(request.str_name, str_my_name, MAX_NAME_LEN);
	request.str_name[MAX_NAME_LEN] = '\0';
	
	/* Initialize sample buffer, by pointing the void pointer within
	* the buffer array to a valid sample memory location. */
	samples_response[0] = JoinResponse__alloc();
	
	samples_position[0] = Position__alloc();
	
	// Record the time now
	uint64_t last_join_req_time = dds_time();
	/* printf("last_join_req_time = %" PRIu64 "\n", last_join_req_time); */
	while (1)
	{
		// If not in a game yet
		pthread_mutex_lock(&mutex);
		int int_player_id_temp = int_player_id;
		pthread_mutex_unlock(&mutex);
		if (int_player_id_temp == -1)
		{
			//Listen for server join response
			rc = dds_take(response_reader, samples_response, infos_response, MAX_SAMPLES, MAX_SAMPLES);
			
			if (rc < 0)
				DDS_FATAL("dds_take: %s\n", dds_strretcode(-rc));

			/* Check if we read some data and it is valid. */
			if ((rc > 0) && (infos_response[0].valid_data))
			{
				JoinResponse *response = (JoinResponse *) samples_response[0];

				// Multiple clients share this reader/topic, so only react
				// to a response that was addressed to the name we requested.
				if (strcasecmp(response->str_name, str_my_name) == 0)
				{
					printf("Join response received\n");
					int int_resp_player_id = response->int_player_id;

					if (int_resp_player_id == 0)
					{
						// Server is full
						printf("Unable to join: server is full\n");
					}
					else if (int_resp_player_id == -1)
					{
						// Name already taken - ask for another one and retry
						printf("Name '%s' is already taken. Enter a different name (max %d chars): ",
							str_my_name, MAX_NAME_LEN);
						fflush(stdout);

						pthread_mutex_lock(&mutex);
						prompt_for_name(str_my_name);
						strncpy(request.str_name, str_my_name, MAX_NAME_LEN);
						request.str_name[MAX_NAME_LEN] = '\0';
						pthread_mutex_unlock(&mutex);

						printf("Trying to join as '%s'...\n", str_my_name);

						// Force an immediate re-send of the join request
						last_join_req_time = 0;
					}
					else
					{
						// printf("Join response player ID = %d\n", response->int_player_id);
						pthread_mutex_lock(&mutex);
						// Update player ID
						int_player_id = int_resp_player_id;
						pthread_mutex_unlock(&mutex);
						printf("Joined as Player %d (%s)\n", int_resp_player_id, str_my_name);
					}
				}
			}
			else
			{
				/* Polling sleep. */
				dds_sleepfor (DDS_MSECS (20));
			}
			
			// Send request every 1s
			// Get the current time
			uint64_t curr_time = dds_time();
			/* printf("curr_time = %" PRIu64 "\n", curr_time); */
			// Get the time difference
			if (curr_time - last_join_req_time > DDS_SECS(1))
			{
				// Update the last req time
				last_join_req_time = curr_time;
				
				// Send join request
				rc = dds_write(join_writer, &request);
				printf("Join request sent (name '%s')\n", request.str_name);
				if (rc != DDS_RETCODE_OK)
				{
					DDS_FATAL("dds_write: %s\n", dds_strretcode(-rc));
				}
			}
		}
		
		// Client is an active player
		else
		{
			pthread_mutex_lock(&mutex);
			char ch_dir_temp = ch_dir;
			pthread_mutex_unlock(&mutex);
			// If there is some direction input
			if(ch_dir_temp != 'n')
			{
				// Create a fresh input
				Input input;
				input.int_player_id = int_player_id;
				
				// Set the direction in the message
				input.ch_direction = ch_dir_temp;			
				// Send input
				rc = dds_write(input_writer, &input);
				// printf("curr_time = %" PRIu64 "\n", dds_time());
				if (rc != DDS_RETCODE_OK)
				{
					DDS_FATAL("dds_write: %s\n", dds_strretcode(-rc));
				}
				printf("Input sent\n");
				// Need to slow down how often input gets sent, otherwise, 
				//the player will move to the window limits immediately
				dds_sleepfor(DDS_MSECS(10));
			}
		}

		// Listen for position
		rc = dds_take(position_reader, samples_position, infos_position, MAX_SAMPLES, MAX_SAMPLES);
		
		if (rc < 0)
			DDS_FATAL("dds_take: %s\n", dds_strretcode(-rc));

		// Check if we read some data and it is valid.
		if ((rc > 0) && (infos_position[0].valid_data))
		{
			printf("Position received\n");
			Position *position = (Position *) samples_position[0];
			// Get the player ID
			int int_position_player_id = position->int_player_id;			
			pthread_mutex_lock(&mutex);
			// Set this player to active
			arr_Players[int_position_player_id-1].b_active = true;
			// Set this player's x
			arr_Players[int_position_player_id-1].x = position->int_x;
			// Set this player's y
			arr_Players[int_position_player_id-1].y = position->int_y;
			// Set this player's name
			strncpy(arr_Players[int_position_player_id-1].str_name, position->str_name, MAX_NAME_LEN);
			arr_Players[int_position_player_id-1].str_name[MAX_NAME_LEN] = '\0';
			pthread_mutex_unlock(&mutex);
		}
	}
}

int main(int argc, char *argv[])
{
	// Which client cert (certs/clientN.pem / clientN.key) this instance
	// authenticates as. Defaults to "1" if not given on the command line.
	if (argc > 1)
	{
		strncpy(str_client_id, argv[1], sizeof(str_client_id) - 1);
		str_client_id[sizeof(str_client_id) - 1] = '\0';
	}
	printf("Using certs/client%s.pem for authentication\n", str_client_id);

	// Ask for the player's name BEFORE anything else happens - this is
	// what gets sent in the join request, so it must be known first.
	printf("Enter your player name (max %d characters): ", MAX_NAME_LEN);
	fflush(stdout);
	prompt_for_name(str_my_name);
	printf("Welcome, %s! Connecting to server...\n", str_my_name);

	pthread_t thread;

    if (pthread_create(&thread, NULL, worker, NULL) != 0)
    {
        perror("pthread_create");
        return 1;
    }
	
	
    if (!glfwInit())
        return 1;

    GLFWwindow *window =
        glfwCreateWindow(WIDTH, HEIGHT, "DDS Client (Click within the window to control)", NULL, NULL);

    if (!window)
    {
        glfwTerminate();
        return 1;
    }

    glfwMakeContextCurrent(window);

    /* Use pixel coordinates */
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    /* glOrtho(0, WIDTH, 0, HEIGHT, -1, 1); */
	glOrtho(0, WIDTH, HEIGHT, 0, -1, 1);

  /*   glMatrixMode(GL_MODELVIEW);
    glLoadIdentity(); */

    while (!glfwWindowShouldClose(window))
    {
		 /* Process keyboard/window events */
        glfwPollEvents();

        /* Non-blocking keyboard input */
        if (glfwGetKey(window, GLFW_KEY_UP) == GLFW_PRESS)
		{
			pthread_mutex_lock(&mutex);
			ch_dir = 'u';
			pthread_mutex_unlock(&mutex);
		}
		
        else if (glfwGetKey(window, GLFW_KEY_DOWN) == GLFW_PRESS)
		{
			pthread_mutex_lock(&mutex);
			ch_dir = 'd';
			pthread_mutex_unlock(&mutex);
		}
		
		else if (glfwGetKey(window, GLFW_KEY_LEFT) == GLFW_PRESS)
		{
			pthread_mutex_lock(&mutex);
			ch_dir = 'l';
			pthread_mutex_unlock(&mutex);
		}
		
		else if (glfwGetKey(window, GLFW_KEY_RIGHT) == GLFW_PRESS)
		{
			pthread_mutex_lock(&mutex);
			ch_dir = 'r';
			pthread_mutex_unlock(&mutex);
		}
		
		else
		{
			pthread_mutex_lock(&mutex);
			ch_dir = 'n';
			pthread_mutex_unlock(&mutex);
		}
		
		glClearColor(1.0f, 1.0f, 1.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
		glColor3f(1.0f, 0.0f, 0.0f);
		
		
		char str_banner[100] = "Waiting for server ...";
		if (int_player_id != -1)
		{
			snprintf(str_banner, sizeof(str_banner), "Player %d (%s)", int_player_id, str_my_name);
		}
		draw_text(320, 20, 3.0f, str_banner);
		
		for(int i = 0; i < MAX_PLAYERS; i++)
		{
			pthread_mutex_lock(&mutex);
			if(arr_Players[i].b_active == true)
			{
				// Label the player with their name
				char str_label[MAX_NAME_LEN + 1];
				snprintf(str_label, sizeof(str_label), "%s", arr_Players[i].str_name);
				/* printf("Coords = %d, %d\n",arr_Players[i].x, arr_Players[i].y);				 */
				/* draw_text(100, 400, 3.0f, str_banner); */
				
				draw_text(arr_Players[i].x, arr_Players[i].y, 3.0f, str_label);				
				
				glBegin(GL_LINE_LOOP);
					glVertex2f(arr_Players[i].x, arr_Players[i].y);
					glVertex2f(arr_Players[i].x, arr_Players[i].y + SQ_WIDTH);
					glVertex2f(arr_Players[i].x + SQ_WIDTH, arr_Players[i].y + SQ_WIDTH);
					glVertex2f(arr_Players[i].x + SQ_WIDTH, arr_Players[i].y);
				glEnd();
			}
			pthread_mutex_unlock(&mutex);			
		}
        glfwSwapBuffers(window);
    }

    glfwDestroyWindow(window);
    glfwTerminate();

    return 0;
}
