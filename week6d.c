//***************************************************************************
// WEEK6.C - homework assignment for Week 6, ECE531, Summer 2019
// Marc Bernard
//
// MUST be compiled and linked in two separate steps to get mySQL working:
// Syntax is EXACTLY as stated below. mysql_config is a literal "mysql_config"
// Tick marks surrounding `mysql_config` are backticks (under the tilde)
// $ gcc -c `mysql_config --cflags` progname.c
// $ gcc -o progname progname.o `mysql_config --libs`
//
// Socket/webserver portion was developed by me.  There will be bugs, there
// will be exploitable areas, there will be security loopholes.
// Attempts to mitigate this are included in handleSQL().
//
// This program requires the mySQL service to be running at the same time.
// Run the following command before kicking off this program:
// $ sudo service mysql start
//
// This program is designed to run on AWS, which only allows port 80 for
// HTTP connections.  You must run this program with sudo.
//
//***************************************************************************


#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <unistd.h>
#include <netdb.h>
#include <sys/time.h>

#include "mysql/include/mysql.h"

#include <signal.h>
#include <sys/stat.h>
#include <syslog.h>

#define MYPORT 80    /* the port users will be connecting to */
#define BACKLOG 10     /* how many pending connections queue will hold */

#define DAEMON_NAME "week6d"
#define OK 0
#define ERR_FORK -1
#define ERR_SETSID -2
#define ERR_CHDIR -3

int                  sockfd, new_fd;  /* listen on sock_fd, new connection on new_fd */
struct  sockaddr_in  my_addr;    /* my address information */
struct  sockaddr_in* their_addr; /* connector's address information */
struct  sockaddr     temp_addr;
int                  sin_size;
//int                  n,i,j;
unsigned int         opt = 1;
fd_set master;    // master file descriptor list
fd_set read_fds;  // temp file descriptor list for select()
int fdmax;        // maximum file descriptor number
struct sockaddr_storage remoteaddr; // client address
socklen_t addrlen;
char resp[4096];
struct timeval tv;

char stringList[50][4096]; // Is 50 lines enough?
int stringCount = 0;
char payload[4096];
char thePath[4096];
int payloadStartingLine;
int theAction;
typedef enum
{
 Action_GET,
 Action_PUT,
 Action_POST,
 Action_DELETE,
 Action_HELP,
 Action_UNKNOWN
} Actions;

MYSQL *conn;

char *sql_server = "localhost";
char *sql_user = "root";
char *sql_password = "";
char *sql_database = "ECE531";

int demonized = 0;
int responseCount = 0;

//*****************************************************************************
// Direct trace statements to either stdout, or syslog, depending on where
// we're running.
//*****************************************************************************
void SAY(char* s, ...)
{
 char _buffer[4096];

  va_list ap;
  va_start(ap, s);
  vsprintf(_buffer, s, ap);

  if (demonized == 1) syslog(LOG_INFO, _buffer);
  else fprintf(stdout, "%s\n", _buffer);

} // End Function SAY()


//*****************************************************************************
//*****************************************************************************
static void _signal_handler(const int signal)
{
 switch (signal)
 {
  case SIGHUP:
    break;
  case SIGTERM:
    syslog(LOG_INFO, "SIGTERM received, exiting");
    closelog();
    exit(OK);
    break;
  default:
    syslog(LOG_INFO, "Unknown signal received");
 }

} // End Function _signal_handler()


//***************************************************************************
//***************************************************************************
int _demon_stuff(void)
{
 demonized = 1;

 openlog(DAEMON_NAME, LOG_PID | LOG_NDELAY | LOG_NOWAIT, LOG_DAEMON);
 SAY("Starting week6d...");

 pid_t pid = fork();
 if (pid < 0) // General fork error - log it and get out
 {
  SAY("Error Attempting fork(%s)\n", strerror(errno));
  return ERR_FORK;
 }
 if (pid > 0) // Parent process gets a PID > 0.  We want to be the child.
 {
  // Not really an error, but we don't want to be the parent.
  // return a zero value so this process will end.
  return OK;
 }

 if (setsid() < -1)
 {
  SAY("Error attempting setsid(%s)\n", strerror(errno));
  return ERR_SETSID;
 }

 close(STDIN_FILENO);
 close(STDOUT_FILENO);
 close(STDERR_FILENO);

 umask(S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);

 if (chdir("/") < 0)
 {
  SAY("Error attempting chdir(%s)\n", strerror(errno));
  return ERR_CHDIR;
 }

 signal(SIGTERM, _signal_handler);
 signal(SIGHUP, _signal_handler);

 return 1; // Non-negative means success

} // End Function _demon_stuff()


//***************************************************************************
// Payload may come in as multiple lines, starting at 'payloadStartingLine'.
// Combine them into a single string.
//***************************************************************************
char* createPayload(void)
{
 char combined[4096];
 static char ret[4096];
 int x, idx, len, currline;

 idx = 0;
 memset(combined, '\0', sizeof(combined));

 // stringCount == 8, payloadStartingLine = 7
 currline = payloadStartingLine; // 7
 while (currline < stringCount) // Make sure we have at least one payload line
 {

  len = strlen(stringList[currline]);
  for (x = 0; x < len; x++)
  {
   combined[idx++] = stringList[currline][x];
  } // End For (x to len)

//  if (currline < (stringCount-1)) // if this isn't the very last line...
//  {
//   combined[idx++] = 0x0D;
//   combined[idx++] = 0x0A;
//  }

  currline++;
 } // End while (currline < stringCount)

 SAY("   incoming payload [%s]", combined);

 memset(ret, '\0', sizeof(ret));
 memcpy(ret, combined, idx);

 return ret;

} // End Function createPayload()


//***************************************************************************
// designed to "escape" things like forward slashes and quotation marks.
// By doubling them up (turning " into "", or turning / into //), SQL will
// recognize to actually put a quotation mark (or slash) into the data stream.
//***************************************************************************
char* doubleUp(char* raw, int replace)
{
 char combined[4096];
 static char ret[4096];
 int x, idx, len;

 idx = 0;
 memset(combined, '\0', sizeof(combined));

  len = strlen(raw);
  for (x = 0; x < len; x++)
  {
   if (raw[x] == replace)
   {
    combined[idx++] = replace;
    combined[idx++] = replace;
   }
   else
   {
    combined[idx++] = raw[x];
   }

  } // End For (x to len)

 memset(ret, '\0', sizeof(ret));
 memcpy(ret, combined, idx);

 return ret;

} // End Function doubleUp()


//***************************************************************************
// Set up the connection the the SQL database.  It is critical that the
// mySQL demon is running at the same time as this program.
//***************************************************************************
int initSQL(void)
{
 conn = mysql_init(NULL);

 /* Connect to database */
 if (!mysql_real_connect(conn, sql_server, sql_user,
     sql_password, sql_database, 0, NULL, 0))
 {
  SAY("SQL Connect Error (%s)", mysql_error(conn));

  return -1;
 }

 return 1;

} // End Function initSQL()


//***************************************************************************
//***************************************************************************
char* badServerID(void)
{
 static char RetVal[256];
 char temp[256];
 memset(temp, '\0', sizeof(temp));

 responseCount++;
 if (responseCount > 9) responseCount = 0;

 if (responseCount == 0) sprintf(temp, "Keep Trying.");
 if (responseCount == 1) sprintf(temp, "Not quite what I had in mind.");
 if (responseCount == 2) sprintf(temp, "You're new at this, aren't you?");
 if (responseCount == 3) sprintf(temp, "You call yourself a hacker?");
 if (responseCount == 4) sprintf(temp, "Well at least take me to dinner first.");
 if (responseCount == 5) sprintf(temp, "Hehe, that tickles.");
 if (responseCount == 6) sprintf(temp, "Don't worry, it happens to everyone.");
 if (responseCount == 7) sprintf(temp, "Too bad, so sad.");
 if (responseCount == 8) sprintf(temp, "I do not think it means what you think it means.");
 if (responseCount == 9) sprintf(temp, "Just what exactly are you looking for?");

 memset(RetVal, '\0', sizeof(RetVal));
 sprintf(RetVal, "%s", temp);
 return RetVal;

} // End Funtion badServerID()


//***************************************************************************
//***************************************************************************
char* goodServerID(void)
{
 static char RetVal[256];
 char temp[256];

 memset(temp, '\0', sizeof(temp));
 responseCount++;
 if (responseCount > 9) responseCount = 0;

 if (responseCount == 0) sprintf(temp, "Ahh, that's the stuff.");
 if (responseCount == 1) sprintf(temp, "Looks like you've read the manual.");
 if (responseCount == 2) sprintf(temp, "IITYWYBMAD");
 if (responseCount == 3) sprintf(temp, "List your top ten U2 songs - Go!");
 if (responseCount == 4) sprintf(temp, "RDWHAHB");
 if (responseCount == 5) sprintf(temp, "I learned VI for this assignment.");
 if (responseCount == 6) sprintf(temp, "This space for rent.");
 if (responseCount == 7) sprintf(temp, "I believe in a higher Bob.");
 if (responseCount == 8) sprintf(temp, "Python? Bah, that's for people who don't know C.");
 if (responseCount == 9) sprintf(temp, "There's only ten of these; they cycle from here.");

 memset(RetVal, '\0', sizeof(RetVal));
 sprintf(RetVal, "%s", temp);
 return RetVal;

} // End Function goodServerID()

//***************************************************************************
// Use theAction to update/retrieve from the SQL database.
// Primary key will be thePath, data will be payload.
// Return what will eventually be the HTTP response to the original request.
//***************************************************************************
char* handleSQL(void)
{
 char responseHeader[256]; // "HTTP/1.1 XXX Reason"
 char responseContentLength[256]; // "Content-Length: XX"
 char responseServerID[256];
 char* responseContentType = "Content-Type: application/json";
 char responseBody[4096];
 static char fullResponse[4096];
 int responseCode;
 char odoa[3];
 char myQuery[256];
 int result;
 //char usePath[4096];
 //char usePayload[4096];
 int shouldContinue = 0;
 char debug[256];
 int tempint;
 int goodOrBad = 0;

 MYSQL_RES*   res;
 MYSQL_ROW    row;
 MYSQL_FIELD* mysqlFields;
 unsigned long numRows;
 unsigned int numFields;
 unsigned long affectedRows;


 odoa[0] = 0x0D;
 odoa[1] = 0x0A;
 odoa[2] = 0x00;

 // Make sure we have all our necessary parts to perform an SQL query:
 if (((theAction == Action_GET) || (theAction == Action_DELETE)) && (strlen(thePath) > 0))
 {
  shouldContinue = 1;
 }
 else if (((theAction == Action_PUT) || (theAction == Action_POST)) && (strlen(thePath) > 0) && (payloadStartingLine > 0))
 {
  shouldContinue = 1;
 }
 else
 {
  shouldContinue = 0;
  sprintf(responseHeader, "HTTP/1.1 400 Bad Request\0");
  sprintf(responseBody, "{\"status\":\"failure\",\"reason\":\"Insufficient Data (no payload/no path/no verb)\"}\0");
  sprintf(responseContentLength, "Content-Length: %d\0", strlen(responseBody));
  goodOrBad = 0;
 }


 if (shouldContinue == 1)
 {

 //~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
 if ((strcmp(thePath, "/")) && (strlen(thePath) == 1)) // Empty path specified
 {
  sprintf(responseHeader, "HTTP/1.1 403 Forbidden\0");
  sprintf(responseBody, "{\"status\":\"failure\",\"reason\":\"No Path Specified\"}\0");
  sprintf(responseContentLength, "Content-Length: %d\0", strlen(responseBody));
  goodOrBad = 0;
 }
 //~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
 else if (strstr(thePath, "*") != 0) // wildcard included in path.
 {
  sprintf(responseHeader, "HTTP/1.1 403 Forbidden\0");
  sprintf(responseBody, "{\"status\":\"failure\",\"reason\":\"Wildcards Not Allowed!\"}\0");
  sprintf(responseContentLength, "Content-Length: %d\0", strlen(responseBody));
  goodOrBad = 0;
 }
 //~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
 else if (theAction == Action_GET) // User wants to GET existing data.
 {
  numRows = 0;
  sprintf(myQuery, "select * from WEEK6 where Path='%s'\0", thePath);
  //fprintf(stdout, "--> [%s]\n", myQuery); // DEBUG ONLY

  if (mysql_query(conn, myQuery)) // != 0
  {
   sprintf(responseHeader, "HTTP/1.1 404 Not Found\0");
   sprintf(responseBody, "{\"status\":\"failure\",\"reason\":\"%s\"}\0", mysql_error(conn));
   sprintf(responseContentLength, "Content-Length: %d\0", strlen(responseBody));
   goodOrBad = 0;
  }
  else
  {
   res = mysql_store_result(conn); // Get the Result Set
   if (res)  // Query was successful.  Do we have any rows in the result?
   {
    numRows = mysql_num_rows(res);
    numFields = mysql_num_fields(res);
   }

   if (numRows <= 0)
   {
    sprintf(responseHeader, "HTTP/1.1 404 Not Found\0");
    sprintf(responseBody, "{\"status\":\"failure\",\"reason\":\"Not Found (%d)\"}\0", mysql_errno(conn));
    sprintf(responseContentLength, "Content-Length: %d\0", strlen(responseBody));
    goodOrBad = 0;
   }
   else
   {
    //fprintf(stdout, "We have %d rows with %d fields\n", numRows, numFields);
    row = mysql_fetch_row(res);
    sprintf(responseHeader, "HTTP/1.1 200 OK\0");
    sprintf(responseBody, "{\"status\":\"success\",\"data\":\"%s\"}\0", row[1]);
    sprintf(responseContentLength, "Content-Length: %d\0", strlen(responseBody));
    goodOrBad = 1;
   }

   if (res) mysql_free_result(res);

  } // End else (myQuery succeeded)

 } // Endif (Action_GET)
 //~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
 else if ((theAction == Action_POST) || (theAction == Action_PUT)) // POST is used to create.  PUT can be either create or update.
 {
  if (theAction == Action_PUT)
  {
   sprintf(myQuery, "update WEEK6 set JSON='%s' where Path='%s'\0", createPayload(), thePath);
  }
  else if (theAction == Action_POST)
  {
   sprintf(myQuery, "insert into WEEK6 (Path, JSON) values ('%s', '%s')\0", thePath, createPayload());
  }

  result = mysql_query(conn, myQuery);
  affectedRows = mysql_affected_rows(conn);

  if (result != 0)
  {
   SAY("POST/PUT Query failed: %s", mysql_error(conn));

    if (theAction == Action_PUT) // UPDATE failed, try again with INSERT
    {
     SAY("PUT failed as an 'update', attempting 'insert' instead");

     sprintf(myQuery, "insert into WEEK6 (Path, JSON) values ('%s', '%s')\0", thePath, createPayload());
     result = mysql_query(conn, myQuery);
     if (result != 0) // Still have an error from either PUT or POST
     {
      SAY("PUT Query failed as insert: %s", mysql_error(conn));
      sprintf(responseHeader, "HTTP/1.1 500 Internal Server Error\0");
      sprintf(responseBody, "{\"status\":\"failure\",\"reason\":\"%s\"}\0", mysql_error(conn));
      sprintf(responseContentLength, "Content-Length: %d\0", strlen(responseBody));
      goodOrBad = 0;
     }
     else
     {
      affectedRows = mysql_affected_rows(conn);
     }
    } // Endif (2nd attempt with PUT as insert)
    else if (theAction == Action_POST) // INSERT failed, try again with UPDATE
    {
     SAY("POST failed as an 'insert', attempting 'update' instead");

     sprintf(myQuery, "update WEEK6 set JSON='%s' where Path='%s'\0", createPayload(), thePath);
     result = mysql_query(conn, myQuery);
     if (result != 0) // Still have an error from either PUT or POST
     {
      sprintf(responseHeader, "HTTP/1.1 500 Internal Server Error\0");
      sprintf(responseBody, "{\"status\":\"failure\",\"reason\":\"%s\"}\0", mysql_error(conn));
      sprintf(responseContentLength, "Content-Length: %d\0", strlen(responseBody));
      goodOrBad = 0;
     }
     else
     {
      affectedRows = mysql_affected_rows(conn);
     }
    } // Endif (2nd attempt with POST as update)

  } // Endif (result != 0)

   if ((affectedRows > 0) && (result == 0))
   {
    SAY("POST/PUT Query Succeeded!");

    sprintf(responseHeader, "HTTP/1.1 200 OK\0");
    sprintf(responseBody, "{\"status\":\"success\",\"key\":\"%s\"}\0", thePath);
    sprintf(responseContentLength, "Content-Length: %d\0", strlen(responseBody));
    goodOrBad = 1;
   }
   else
   {
    SAY("insert command failed for either PUT or POST, no action taken");

    sprintf(responseHeader, "HTTP/1.1 404 Not Found\0");
    sprintf(responseBody, "{\"status\":\"failure\",\"reason\":\"Not Found (%d)\"}\0", mysql_errno(conn));
    sprintf(responseContentLength, "Content-Length: %d\0", strlen(responseBody));
    goodOrBad = 0;
   }


 } // Endif (Action_POST) || (Action_PUT)
 //~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
 else if (theAction == Action_DELETE) // User wants to delete data.
 {
  // We've already handled an empty path condition, so this *SHOULD* be safe.
  //sprintf(usePath, "%s\0", doubleUp(thePath, 0x2F));
  sprintf(myQuery, "delete from WEEK6 where Path='%s'\0", thePath);
  if (mysql_query(conn, myQuery)) // != 0
  {
   sprintf(responseHeader, "HTTP/1.1 500 Internal Server Error\0");
   sprintf(responseBody, "{\"status\":\"failure\",\"reason\":\"%s\"}\0", mysql_error(conn));
   sprintf(responseContentLength, "Content-Length: %d\0", strlen(responseBody));
   goodOrBad = 0;
  }
  else
  {
   affectedRows = mysql_affected_rows(conn);
   if (affectedRows > 0)
   {
    sprintf(responseHeader, "HTTP/1.1 200 OK\0");
    sprintf(responseBody, "{\"status\":\"success\",\"information\":\"data deleted\"}\0");
    sprintf(responseContentLength, "Content-Length: %d\0", strlen(responseBody));
    goodOrBad = 1;
   }
   else
   {
    sprintf(responseHeader, "HTTP/1.1 404 Not Found\0");
    sprintf(responseBody, "{\"status\":\"failure\",\"reason\":\"Not Found (%d)\"}\0", mysql_errno(conn));
    sprintf(responseContentLength, "Content-Length: %d\0", strlen(responseBody));
    goodOrBad = 0;
   }
  }
 } // Endif (Action_DELETE)

 } // Endif (shouldContinue == 1)


 sprintf(responseServerID, "%s\0", (goodOrBad == 0) ? badServerID() : goodServerID());

 if (demonized == 0) printf("responding with [%s]\n", responseServerID);

 sprintf(fullResponse, "%s%s%s%s%s%s%s%s%s%s\0",
                         responseHeader, odoa,
                         responseServerID, odoa,
                         responseContentType, odoa,
                         responseContentLength, odoa,
                         odoa, responseBody);

 //fprintf(stdout, "%s\n", fullResponse); // Debug Only

 return fullResponse;



} // End Function handleSQL()


//***************************************************************************
// Assume the line always has the following format.
// POST /api/login HTTP/1.1
//
// A more robust solution would be to check that the verb, path, and HTTP
// aren't interchanged, but things are much easier if we assume a consistent
// model
//***************************************************************************
char* getPath(char* data)
{
 int x, y, len;
 char element[4096];
 static char retval[4096];

 len = strlen(data);
 y = 0;
 memset(element, '\0', sizeof(element));

 for (x = 0; x < len; x++)
 {
  if (data[x] == 0x20) // If we hit a space
  {
   while ((data[x] == 0x20) && (x < len)) // keep walking the line until we hit a non-space
   {
    x++;
   }
   break;
  }

 } // End for (x to len)

 if (x < len) // if we didn't hit the end of the line looking for a non-space.
 {
  while ((data[x] != 0x20) && (x < len)) // Now keep going until we DO find a space
  {
   element[y++] = data[x++];
  }
 } // Endif (x < len)

 memset(retval, '\0', sizeof(retval));
 memcpy(retval, element, y); // use memcpy so we don't run into issues with sprintf
                             // trying to process '%' and '\' in the pathname.
 return retval;

} // End Function getPath()


//***************************************************************************
//
// 50 4F 53 54 20 2F 61 70 69 2F 6C 6F 67 69 6E 20   POST /api/login
// 48 54 54 50 2F 31 2E 31 0D 0A 48 6F 73 74 3A 20   HTTP/1.1..Host:
// 33 2E 31 33 2E 31 35 37 2E 37 39 0D 0A 55 73 65   3.13.157.79..Use
// 72 2D 41 67 65 6E 74 3A 20 63 75 72 6C 2F 37 2E   r-Agent: curl/7.
// 36 35 2E 30 0D 0A 41 63 63 65 70 74 3A 20 2A 2F   65.0..Accept: */
// 2A 0D 0A 43 6F 6E 74 65 6E 74 2D 54 79 70 65 3A   *..Content-Type:
// 20 61 70 70 6C 69 63 61 74 69 6F 6E 2F 6A 73 6F    application/jso
// 6E 0D 0A 43 6F 6E 74 65 6E 74 2D 4C 65 6E 67 74   n..Content-Lengt
// 68 3A 20 33 35 0D 0A 0D 0A 7B 22 75 73 65 72 6E   h: 35....{"usern
// 61 6D 65 22 3A 22 78 79 7A 22 2C 22 70 61 73 73   ame":"xyz","pass
// 77 6F 72 64 22 3A 22 78 79 7A 22 7D 00 00 00 00   word":"xyz"}....
//
// len = 172

// POST /api/login HTTP/1.1..
// 01234567890123456789012345

//
// Break the incoming message into individual lines. Each line is delimited
// by 0x0D 0x0A (by default).  Once we have our lines, search them for the
// desired verb (which should contain the path on the same line), and the
// payload.
//***************************************************************************
void processRequest(char* data, int len)
{
 char aHeaderLine[4096];
 int x, y, idx;
 int haveNewline = 0;
 int shouldContinue = 1;

 idx = 0;
 x = 0;
 stringCount = 0;

 while (x < (len-2))
 {
  memset(aHeaderLine, '\0', sizeof(aHeaderLine));
  y = 0;
  haveNewline = 0;
  for (x = idx; x < (len-2); x++)
  {
   if ((data[x] == 0x0D) && (data[x+1] == 0x0A))
   {
    haveNewline = 1;
    idx = x+2;
    //fprintf(stdout, "Newline found, idx [%d], len [%d]\n", idx, len);
    break;
   }
   else
   {
    aHeaderLine[y++] = data[x];
   }
  } // End for (x to len-2)

  if (haveNewline == 0) // This will probably be the payload line, but we need
  {                     // to check that the stringList line before it is blank
   aHeaderLine[y++] = data[x];
   aHeaderLine[y++] = data[x+1];
   memset(stringList[stringCount], '\0', sizeof(stringList[stringCount]));
   memcpy(stringList[stringCount], aHeaderLine, y);
   stringCount++;
  }
  else
  {
   memset(stringList[stringCount], '\0', sizeof(stringList[stringCount]));
   memcpy(stringList[stringCount], aHeaderLine, y);
   stringCount++;
  }

 } // End while (x < len-2)

 for (x = 0; x < stringCount; x++)
 {
  //fprintf(stdout, "%d = [%s], %d\n", x, stringList[x], strlen(stringList[x]));
  if (demonized == 1) syslog(LOG_INFO, "%d = [%s]", x, stringList[x]);
  else printf("%d = [%s]\n", x, stringList[x]);
 }


 theAction = Action_UNKNOWN;
 payloadStartingLine = -1;
 memset(thePath, '\0', sizeof(thePath));

 for (x = 0; x < stringCount; x++)
 {

  if ((strstr(stringList[x], "GET ") != 0) && (theAction == Action_UNKNOWN))
  {
   //fprintf(stdout, "incoming action is GET\n"); // Debug Only
   SAY("   GET requested...");
   theAction = Action_GET;
   sprintf(thePath, "%s\0", getPath(stringList[x]));
  }
  else if ((strstr(stringList[x], "PUT ") != 0) && (theAction == Action_UNKNOWN))
  {
   //fprintf(stdout, "incoming action is PUT\n"); // Debug Only
   SAY("   PUT requested...");
   theAction = Action_PUT;
   sprintf(thePath, "%s\0", getPath(stringList[x]));
  }
  else if ((strstr(stringList[x], "POST ") != 0) && (theAction == Action_UNKNOWN))
  {
   //fprintf(stdout, "incoming action is POST\n"); // Debug Only
   SAY("   POST requested...");
   theAction = Action_POST;
   sprintf(thePath, "%s\0", getPath(stringList[x]));
  }
  else if ((strstr(stringList[x], "DELETE ") != 0) && (theAction == Action_UNKNOWN))
  {
   //fprintf(stdout, "incoming action is DELETE\n"); // Debug Only
   SAY("   DELETE requested...");
   theAction = Action_DELETE;
   sprintf(thePath, "%s\0", getPath(stringList[x]));
  }
  else if ((strlen(stringList[x]) == 0) && (payloadStartingLine < 0))
  {
   // Each of the stringList lines terminates with a newline.  HTTP convention
   // is that a blank line inidcates the next line is the payload data.
   payloadStartingLine = x+1;
  }

  if ((theAction != Action_UNKNOWN) && (payloadStartingLine > 0)) break;

 } // End for (x to stringCount)

 SAY("   Desired path is [%s]", thePath);

 //fprintf(stdout, "incoming path is [%s].", thePath); // Debug Only
 //fprintf(stdout, "payload starts at line [%d]", payloadStartingLine); // Debug Only


} // End Function processRequest()


//***************************************************************************
//***************************************************************************
int setupServer(void)
{
  tv.tv_sec = 0;  // timeout value to wait for socket to have data
  tv.tv_usec = 500;

  if ((sockfd = socket(AF_INET, SOCK_STREAM, 0)) == -1)
  {
   SAY("Error creating socket");
   perror("socket");
   return -1;
  }
  //else fprintf(stdout, "created socket...\n");

  setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
  //fprintf(stdout, "Set sockopt...\n");

  my_addr.sin_family = AF_INET;         /* host byte order */
  my_addr.sin_port = htons(MYPORT);     /* short, network byte order */
  my_addr.sin_addr.s_addr = INADDR_ANY; /* auto-fill with my IP */
  bzero(&(my_addr.sin_zero), 8);        /* zero the rest of the struct */

  if (bind(sockfd, (struct sockaddr *)&my_addr, sizeof(struct sockaddr)) == -1)
  {
   SAY("Error binding socket");
   perror("bind");
   return -2;
  }
  //fprintf(stdout, "bind socket...\n");


  if (listen(sockfd, BACKLOG) == -1)
  {
   SAY("Error listening to socket");
   perror("listen");
   return -3;
  }
  //fprintf(stdout, "listening...\n");


  // add the listener to the master set
  FD_SET(sockfd, &master);

  // keep track of the biggest file descriptor
  fdmax = sockfd;

  return 0;

} // End Function setupServer()


//***************************************************************************
//***************************************************************************
int pollServer(void)
{
 char buf[4096];    // buffer for client data
 int nbytes = 0;
 int retVal;

 //fprintf(stdout, "checking for incoming data...\n");

 read_fds = master; // copy it
 if (select(fdmax+1, &read_fds, NULL, NULL, &tv) == -1)
 {
  SAY("Error selecting socket");
  perror("select");
  return -1;
 }

 // run through the existing connections looking for data to read
 if (FD_ISSET(sockfd, &read_fds)) // we got one!!
 {
  // handle new connections
  addrlen = sizeof temp_addr;
  new_fd = accept(sockfd, (struct sockaddr *)&temp_addr, &addrlen);

  if (new_fd == -1)
  {
   SAY("Error accepting socket");
   perror("accept");
   return -2;
  }
  else
  {
   their_addr = (struct sockaddr_in*)&temp_addr;
   //fprintf(stdout, "Incoming connection from [%s]...\n", inet_ntoa(their_addr->sin_addr));
   SAY("Incoming connection from [%s]...", inet_ntoa(their_addr->sin_addr));

   memset(buf, '\0', sizeof(buf));

   if ((nbytes = recv(new_fd, buf, sizeof buf, 0)) <= 0)
   {
    close(new_fd); // bye!

    // got error or connection closed by client
    if (nbytes == 0)
    {
     // connection closed
     //fprintf(stdout, "selectserver: socket %d hung up\n", i);
     SAY("Unexpected Client Disconnect");
    }
    else // -1 means no message available (EWOULDBLOCK is set)
    {
     SAY("No incoming message");
     //perror("recv"); // "Connection Reset by Peer"
     //return -3;
    }

   }
   else // have data to read
   {
    processRequest(buf, nbytes);

    //fprintf(stdout, "call handleSQL\n"); // Debug Only
    sprintf(resp, "%s\0", handleSQL()); // resp will be a fully-formed HTTP 200, etc

    send(new_fd, resp, strlen(resp), 0);
    close(new_fd); // bye!

    return nbytes;

   } // End else (we read something)

  } // End else (accept succeeded)

 } // Endif (FD_ISSET(sockfd))

 return 0;

} // End Function pollServer()


//***************************************************************************
//***************************************************************************
int main(int argc, char* argv[])
{
 int ret;
 int shouldDemonize = 1;

 demonized = 0;

 for (int x = 0; x < argc; x++)
 {
  //printf("Arg %d = [%s]\n", x, argv[x]);
  if ((strcmp(argv[x], "-s") == 0) || (strcmp(argv[x], "--shell") == 0))
  {
   shouldDemonize = 0;
   break;
  }
 }


 if (shouldDemonize == 1)
 {
  if ((ret = _demon_stuff()) <= 0) return ret;
 }
 else
 {
  SAY("running in shell");
 }


 if ((ret = initSQL()) < 0) return ret;

 if ((ret = setupServer()) < 0) return ret;

 while (1)
 {
  if ((ret = pollServer()) < 0) return ret;
  sleep(1);
 } // END while (1)


 return 0;

} // End Function main()