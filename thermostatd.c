#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <syslog.h>
#include <time.h>
#include <errno.h>
#include <ctype.h>
#include <curl/curl.h>

#define OK         0
#define INIT_ERR   -1
#define REQ_ERR    -2
#define CONFIG_ERR -3
#define TIME_ERR   -4
#define TEMP_ERR   -5

#define DAEMON_NAME "thermostatd"
#define ERR_FORK   -6
#define ERR_SETSID -7
#define ERR_CHDIR  -8

#define URL_TEST       "http://localhost:8000"
#define URL_PRODUCTION "http://192.168.1.116:80" // Needed when using ARM
char defaultURL[100];

// This is a holdover from homework.c.  We'll still use GET, PUT and HELP for this program.
int Action;
typedef enum
{
 Action_GET,
 Action_PUT,
 Action_POST,
 Action_DELETE,
 Action_HELP
} Actions;

// Integers to hold programming start times:
int morningStart, afternoonStart, nightStart;

// Integers to hold desired temperatures for each segment of the day.
int morningTemp, afternoonTemp, nightTemp;

// current Time and Temperature: (505) 247-1611 for the Burquenos
int currTime, currTemp;

#define CONFIG_FILENAME_LENGTH 1000
char configFile[CONFIG_FILENAME_LENGTH];


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


/*****************************************************************************/
/*****************************************************************************/
void _uppercase(char* lowercase)
{
 int x, len;

 len = strlen(lowercase);

 for (x = 0; x < len; x++)
 {
  if ((lowercase[x] >= 0x61) && (lowercase[x] <= 0x7A))
  {
   lowercase[x] -= 0x20;
  }
 } // End for (x to len)

} // End Function _uppercase()


//*****************************************************************************
// We're expecting aTime to have the format "HH:MM".  We need to be able to
// handle situations where HH or MM are not two digits each, or not numeric.
//*****************************************************************************
int _convertTimeToInteger(char* aTime)
{
 char Hour[3];
 char Minute[3];
 int ret;
 int x, len, colon;

 len = strlen(aTime);

 if (len != 5) return -1; // if aTime isn't the right length, get out.

 // Find the location of the colon:
 colon = -1;
 for (x = 0; x < len; x++)
 {
  if (aTime[x] == ':')
  {
   colon = x;
   break;
  }
 } // End For (x to len)

 // HH:MM
 // 01234

 if (colon != 2) return -1; // If colon is not where we expect it, get out.

 if (isDigit(aTime[0]) == 0) return -1; // If any of the parts of
 if (isDigit(aTime[1]) == 0) return -1; // HH or MM are non-numeric
 if (isDigit(aTime[3]) == 0) return -1; // get out.
 if (isDigit(aTime[4]) == 0) return -1;

 // Syntax is OK, now check that HH is in the range 00-23 and
 // MM is in the range 00-59
 memset(Hour, '\0', sizeof(Hour));
 memset(Minute '\0', sizeof(Minute));

 memcpy(Hour, aTime, 2);
 memcpy(Minute, aTime+3, 2);

 if (atoi(Hour) > 23) return -1;   // Either HH or MM out of range,
 if (atoi(Minute) > 59) return -1; // then get out.

 // Everything within range. Compute an actual time and express it in
 // minutes:
 ret = atoi(Hour);
 ret *= 60;
 ret += atoi(Minute);

 return ret;

} // End Function _convertToInteger()


//*****************************************************************************
//*****************************************************************************
int readConfigFile(void)
{
 FILE* aFile;
 char aLine[100];
 int len, x, idx;
 int OnLeft = 0;
 char LeftSide[100];
 char RightSide[100];

 // Initialize our values:
 morningStart = afternoonStart = nightStart = -1;
 morningTemp = afternoonTemp = nightTemp = -1;
 sprintf(defaultURL, "%s\0", URL_TEST);

 // If we didn't have a config file passed in from command line,
 // then use a default:
 if (strlen(configFile) == 0)
 {
  sprintf(configFile, "/etc/thermostat/.config\0");
 }

 aFile = fopen(configFile, "rb");
 if (aFile)
 {
  while (!feof(aFile))
  {
   memset(aLine, 0, sizeof(aLine));
   fgets(aLine, 100, aFile);

   // Remove any newline characters:
   len = strlen(aLine);
   for (x = 0; x < len; x++)
   {
    if ((aLine[x] == 0x0d) || (aLine[x] == 0x0a)) aLine[x] = 0x00;
   }
   len = strlen(aLine); // Get the adjusted length


   if ((aLine[0] != '#') && (len > 0)) // Ignore comments and empty lines
   {
     OnLeft = 1;
     idx = 0;
     memset(LeftSide, '\0', sizeof(LeftSide));
     memset(RightSide, '\0', sizeof(RightSide));
     // Collect the contents of the line, looking for an equal sign '=' to
     // delineate the left side (variable name) with the right side (value)
     for (x = 0; x < len; x++)
     {
      if ((aLine[x] != ' ') && (aLine[x] != '=')) // Ignore spaces in the line
      {
       if (OnLeft == 1)
       {
        LeftSide[idx++] = aLine[x];
       }
       else
       {
        RightSide[idx++] = aLine[x];
       }
      }
      if (aLine[x] == '=')
      {
       OnLeft = 0;
       idx = 0;
      }
     }

     _uppercase(LeftSide);  // For consistency, convert
     _uppercase(RightSide); // everything to uppercase.

     // Now evaluate the contents of LeftSide and set our
     // default variables accordingly:
     if (strcmp(LeftSide, "MORNINGSTART") == 0)
     {
      // We expect the start times to come in as HH:MM.
      // Convert this to an integer value, expressed in minutes:
      morningStart = _convertTimeToInteger(RightSide);
     }

     if (strcmp(LeftSide, "AFTERNOONSTART") == 0)
     {
      afternoonStart = _convertTimeToInteger(RightSide);
     }

     if (strcmp(LeftSide, "NIGHTSTART") == 0)
     {
      nightStart = _convertTimeToInteger(RightSide);
     }

     if (strcmp(LeftSide, "MORNINGTEMP") == 0)
     {
      morningTemp = atoi(RightSide);
      if (morningTemp > 100) morningTemp = -1;
     }

     if (strcmp(LeftSide, "AFTERNOONTEMP") == 0)
     {
      afternoonTemp = atoi(RightSide);
      if (afternoonTemp > 100) afternoonTemp = -1;
     }

     if (strcmp(LeftSide, "NIGHTTEMP") == 0)
     {
      nightTemp = atoi(RightSide);
      if (nightTemp > 100) nightTemp = -1;
     }

     if (strcmp(LeftSide, "DEFAULTURL") == 0)
     {
      sprintf(defaultURL, "%s\0", RightSide);
     }

   } // Endif (Not a Comment)

  } // End while (!feof)

  fclose(aFile);

 } // Endif (File opened OK)
 else
 {
  return CONFIG_ERR; // Somethig wrong with the config file.
                     // inform main() and exit the program.
 }

 if ((morningStart == -1) || (afternoonStart == -1) || (nightStart == -1)) return TIME_ERR;
 if ((morningTemp == -1) || (afternoonTemp = -1) || (nightTemp == -1)) return TEMP_ERR;

 return 0; // Indicate no errors

} // End Function readConfigFile()


//*****************************************************************************
//*****************************************************************************
int main(int argc, char* argv[])
{
 CURL* curl;
 CURLcode res;
 int x;
 char* theURL;
 int theAction = Action_UNKNOWN;
 int ret;

 // ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
 // DAEMON SETUP STUFF:
 // ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
 openlog(DAEMON_NAME, LOG_PID | LOG_NDELAY | LOG_NOWAIT, LOG_DAEMON);
 syslog(LOG_INFO, "Starting sampled...");

 pid_t pid = fork();
 if (pid < 0) // General fork error - log it and get out
 {
  syslog(LOG_ERR, "Error Attempting fork(%s)\n", strerror(errno));
  return ERR_FORK;
 }
 if (pid > 0) // Parent process gets a PID > 0.  We want to be the child.
 {
  return OK;
 }

 if (setsid() < -1)
 {
  syslog(LOG_ERR, "Error attempting setsid(%s)\n", strerror(errno));
  return ERR_SETSID;
 }

 close(STDIN_FILENO);
 close(STDOUT_FILENO);
 close(STDERR_FILENO);

 umask(S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);

 if (chdir("/") < 0)
 {
  syslog(LOG_ERR, "Error attempting chdir(%s)\n", strerror(errno));
  return ERR_CHDIR;
 }

 signal(SIGTERM, _signal_handler);
 signal(SIGHUP, _signal_handler);


 // ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
 // PROCESS THE COMMAND LINE:
 // ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
 memset(configFile, '\0', sizeof(configFile));
 if (argc >= 2)
 {
  for (x = 1; x < argc; x++)
  {
   if ((strcmp(argv[x], "-h") == 0) || (strcmp(argv[x], "-help") == 0))
   {
    theAction = Action_HELP;
   }
   else if ((strcmp(argv[x], "-c") == 0) || (strcmp(argv[x], "-config") == 0))
   {
    if (argc >= (x+2)) // Remember, index of argv[0] is count 1.  If we hit "-c" at argv[1], we need to make sure argc is at least 3
    {
     strncpy(configFile, argv[x+1], CONFIG_FILENAME_LENGTH-1);
     x++; // skip over it in the loop so we don't re-process it
    }
   }

  } // End for (x to argc)

 } // Endif (argc >= 2)

 // User requested help - show help screen and get out.
 if (theAction == Action_HELP)
 {
  performHelp();
  return OK;
 }

 // Read the config file.  Get out if we have an error.
 if ((ret = readConfigFile()) < 0) return ret;

 // Everything checks out - start heating the room!
 runThermostat();

 // In theory, we should never reach this line of code.
 return OK;

} // End Function main()