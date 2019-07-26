#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
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
#define OFF        0
#define ON         1
#define UNKNOWN    -1
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
#define URL_PRODUCTION "http://3.13.157.79:80" // Needed when using AWS
char defaultURL[100];

// This is a holdover from homework.c.  We'll still use GET, PUT and HELP for this program.
typedef enum
{
 Action_GET,
 Action_PUT,
 Action_POST,
 Action_DELETE,
 Action_HELP,
 Action_UNKNOWN
} Actions;
int theAction = Action_UNKNOWN;

CURL* curl;
CURLcode res;

// Integers to hold programming start times:
int morningStart, afternoonStart, nightStart;

// Integers to hold desired temperatures for each segment of the day.
int morningTemp, afternoonTemp, nightTemp;

// current Time and Temperature: (505) 247-1611 for the Burquenos
int currTime, currTemp;

int heaterStatus; // Will be either OFF or ON

#define CONFIG_FILENAME_LENGTH 1000
char configFile[CONFIG_FILENAME_LENGTH];

struct MemoryStruct {
  char *memory;
  size_t size;
};

int demonized = 0;


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
static size_t WriteMemoryCallback(void *contents, size_t size, size_t nmemb, void *userp)
{
  size_t realsize = size * nmemb;
  struct MemoryStruct *mem = (struct MemoryStruct *)userp;

  char *ptr = realloc(mem->memory, mem->size + realsize + 1);
  if(ptr == NULL) {
    /* out of memory! */
    SAY("WriteCallback - not enough memory (realloc returned NULL)");
    return 0;
  }

  mem->memory = ptr;
  memcpy(&(mem->memory[mem->size]), contents, realsize);
  mem->size += realsize;
  mem->memory[mem->size] = 0;

  return realsize;
}

//*****************************************************************************
// Allow the program to be stopped gracefully
//*****************************************************************************
static void _signal_handler(const int signal)
{
 switch (signal)
 {
  case SIGHUP:
    break;
  case SIGTERM:
    SAY("SIGTERM received, exiting");
    closelog();
    exit(OK);
    break;
  default:
    SAY("Unknown signal received");
 }

} // End Function _signal_handler()


//***************************************************************************
//***************************************************************************
int _demon_stuff(void)
{
 demonized = 1;

 openlog(DAEMON_NAME, LOG_PID | LOG_NDELAY | LOG_NOWAIT, LOG_DAEMON);
 SAY("Starting thermostat...");

 pid_t pid = fork();
 if (pid < 0) // General fork error - log it and get out
 {
  SAY("Error Attempting fork(%s)\n", strerror(errno));
  return ERR_FORK;
 }
 if (pid > 0) // Parent process gets a PID > 0.  We want to be the child.
 {
  // Not really an error, but we don't want to be the parent.
  // return a sero value so this process will end.
  return 0;
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


//*****************************************************************************
// Convert any lowercase letters in a string into upppercase
//*****************************************************************************
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
// aResponse will have the format of
// "status":"success","data":"07:00" (including quotes)
// We want to return the value of the "data" portion
//*****************************************************************************
char* parseValue(char* aResponse)
{
 int x, len, idx;
 char value[256];
 static char RetVal[256];

 memset(value, '\0', sizeof(value));

 len = strlen(aResponse); // 31

 for (x = 0; x < len-4; x++)
 {

  // "status":"success","data":"07:00"}
  // 01234567890123456789012345678901
  //           1         2         3

  if ((aResponse[x] == 'd') && (aResponse[x+1] == 'a') && (aResponse[x+2] == 't') && (aResponse[x+3] == 'a'))
  {
   idx = x+7;           // 27
   if ((len-2) >= idx)  // (32-2) >= 27 ?
   {
    memcpy(value, aResponse+(idx), (len-idx)-2);
    //SAY("parsed value from server = [%s]\n", value);  // TODO REMOVE
   }
   break;
  }
 }

 len = strlen(value);
 memset(RetVal, '\0', sizeof(RetVal));
 memcpy(RetVal, value, len);
 return RetVal;

} // End Function parseValue()


//*****************************************************************************
// We're expecting aTime to have the format "HH:MM".  We need to be able to
// handle situations where HH or MM are not two digits each, or not numeric.
//*****************************************************************************
int _convertTimeToInteger(char* aTime)
{
 char Hour[3];
 char Minute[3];
 int ret;
 int x, len;

 len = strlen(aTime);

 if (len != 5)
 {
  SAY("Convert Time, Length is not 5, returning\n");
  return -1; // if aTime isn't the right length, get out.
 }

 // HH:MM
 // 01234

 if (aTime[2] != ':')
 {
  SAY("Convert Time, offset 2 is [%c], returning\n", aTime[2]);
  return -1; // If colon is not where we expect it, get out.
 }

 // If any parts of HH or MM are non-numeric, get out.
 if (isdigit(aTime[0]) == 0)
 {
  SAY("Convert Time, offset 0 is [%c], returning\n", aTime[0]);
  return -1;
 }
 if (isdigit(aTime[1]) == 0)
 {
  SAY("Convert Time, offset 1 is [%c], returning\n", aTime[1]);
  return -1;
 }
 if (isdigit(aTime[3]) == 0)
 {
  SAY("Convert Time, offset 4 is [%c], returning\n", aTime[3]);
  return -1;
 }
 if (isdigit(aTime[4]) == 0)
 {
  SAY("Convert Time, offset 4 is [%c], returning\n", aTime[4]);
  return -1;
 }

 // Syntax is OK, now check that HH is in the range 00-23 and
 // MM is in the range 00-59
 memset(Hour, '\0', sizeof(Hour));
 memset(Minute, '\0', sizeof(Minute));

 memcpy(Hour, aTime, 2);
 memcpy(Minute, aTime+3, 2);

 if (atoi(Hour) > 23)
 {
  SAY("Convert Time, Hour [%d], returning\n", atoi(Hour));
  return -1;   // Either HH or MM out of range,
 }
 if (atoi(Minute) > 59)
 {
  SAY("Convert Time, Minute [%d], returning\n", atoi(Minute));
  return -1; // then get out.
 }

 // Everything within range. Compute an actual time and express it in
 // minutes:
 ret = atoi(Hour);
 ret *= 60;
 ret += atoi(Minute);

 return ret;

} // End Function _convertTimeToInteger()


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
 sprintf(defaultURL, "%s\0", URL_PRODUCTION);

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
     //SAY("[%s] = [%s]\n", LeftSide, RightSide);                        // TODO REMOVE

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
      //SAY("Morning Temp [%d]\n", morningTemp);
     }

     if (strcmp(LeftSide, "AFTERNOONTEMP") == 0)
     {
      afternoonTemp = atoi(RightSide);
      if (afternoonTemp > 100) afternoonTemp = -1;
      //SAY("Afternoon Temp [%d]\n", afternoonTemp);
     }

     if (strcmp(LeftSide, "NIGHTTEMP") == 0)
     {
      nightTemp = atoi(RightSide);
      if (nightTemp > 100) nightTemp = -1;
      //SAY("Night Temp [%d]\n", nightTemp);
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
 if ((morningTemp == -1) || (afternoonTemp == -1) || (nightTemp == -1)) return TEMP_ERR;

 // Do we care if morningStart < afternoonStart < nightStart ?
 // Do we care if there's time between nightStart and Midnight?
 // How about time between Midnight and morningStart?

 return 0; // Indicate no errors

} // End Function readConfigFile()


//*****************************************************************************
// Read the current temperature from the input file /var/log/temperature.
// Assume contents of the file are simply numeric, and represent the current
// temperature.  Allow up to 3 digits for the value. Allow negative values.
//*****************************************************************************
int getCurrentTemperature(void)
{
 char currTemp[4];
 int idx, x;
 FILE* inputFile;
// char inFileName[] = "/var/log/temperature";
 char inFileName[] = "/tmp/temp"; // Make these match what tc_main is using.


 inputFile = fopen(inFileName, "rb");
 if (inputFile)
 {
  memset(currTemp, '\0', sizeof(currTemp));
  idx = 0;
  while ((!feof(inputFile)) && (idx < 3))
  {
   currTemp[idx++] = fgetc(inputFile);
  }

  fclose(inputFile);

 } // Endif (inputFile opened OK)
 else
 {
  SAY("Unable to open temperature file /tmp/temp");
  return -1;
 }

 for (x = 0; x < idx; x++)
 {
  if (currTemp[x] == '.') currTemp[x] = '\0';
 }

 idx = strlen(currTemp);
 for (x = 0; x < idx; x++)
 {
  // Return an error for anything non-numeric.  Allow negative numbers.
  if ((isdigit(currTemp[x]) == 0) && (currTemp[x] != '-')) return -1;
 }

 return atoi(currTemp);

} // End Function getCurrentTemperature()


//*****************************************************************************
//*****************************************************************************
void updateServerStatus(char* onOrOff)
{
 char aURL[500];
 int retval = -1;

 memset(aURL, '\0', sizeof(aURL));

 sprintf(aURL, "%s/STATUS", defaultURL);
 //SAY("Update thermostat status using URL %s\n", aURL);

  curl = curl_easy_init();
  if(curl)
  {
    curl_easy_setopt(curl, CURLOPT_URL, aURL); // 3.13.157.79
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L); // If we're not connected in 5 seconds, get out
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L); // If we connect, but don't get a response in 5 seconds, get out.
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, onOrOff);

    res = curl_easy_perform(curl);
    if(res != CURLE_OK)
    {
     SAY("curl GET failed: %s\n", curl_easy_strerror(res));
    }

   curl_easy_cleanup(curl);
  } // Endif (curl)

} // End Function updateServerStatus()


//*****************************************************************************
// Compare current heaterStatus to parameter OnOff.  If there's a change,
// write the change and the time it happened to the output file.
//*****************************************************************************
void toggleHeater(int OnOff, int currTemp)
{
 FILE* outputFile;
 //char outFileName[] = "/var/log/heater";
 char outFileName[] = "/tmp/status"; // Make these match what tc_main is using.
 time_t T;

 if (heaterStatus != OnOff) // Only do something if there's a change.
 {
  heaterStatus = OnOff; // Change the status

  // Open file for overwrite.  Create if it doesn't exist yet.
  outputFile = fopen(outFileName, "wb+");
  if (outputFile)
  {
   T = time(NULL);
   // Program requirements only asked for ON/OFF with the posix timestamp.
   fprintf(outputFile, "%s : %ld\r\n", ((OnOff == ON) ? "ON":"OFF"), T);
   fclose(outputFile);
  } // Endif (outputFile opened OK)
  else
  {
   SAY("Error opening status file");
  }

  // Log the change to /var/log/messages, just for good measure. Include
  // current temperature.  syslog automatically adds the timestamp.
  SAY("Current temperature [%d], turning heater [%s]", currTemp, ((OnOff == ON) ? "ON":"OFF"));

  // Need something here to update HTTP file.
  updateServerStatus(((OnOff == ON) ? "ON":"OFF"));

 } // Endif (heaterStatus != OnOff)

} // End Function toggleHeater()


//*****************************************************************************
//*****************************************************************************
int pollServer(char* label, int isTime)
{
 char aURL[500];
 struct MemoryStruct chunk;
 int retval = -1;

 memset(aURL, '\0', sizeof(aURL));

 //SAY("polling server...\n");

 //SAY("allocating chunk\n");
 chunk.memory = malloc(1);  /* will be grown as needed by the realloc above */
 chunk.size = 0;    /* no data*/

 sprintf(aURL, "%s/%s", defaultURL, label);
 //SAY("using URL %s\n", aURL);

  curl = curl_easy_init();
  if(curl)
  {
    curl_easy_setopt(curl, CURLOPT_URL, aURL); // 3.13.157.79
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L); // If we're not connected in 5 seconds, get out
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L); // If we connect, but don't get a response in 5 seconds, get out.
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteMemoryCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&chunk);

    res = curl_easy_perform(curl);
    if(res != CURLE_OK)
    {
     fprintf(stderr, "curl GET failed: %s\n", curl_easy_strerror(res));
    }
    else
    {
     //SAY("%lu bytes retrieved\n", (unsigned long)chunk.size); // TODO REMOVE
     //SAY("raw data = [%s]\n", chunk.memory);                  // TODO REMOVE
     if (strstr(chunk.memory, "success") != 0)
     {
      if (isTime == 1)
      {
       retval = _convertTimeToInteger(parseValue(chunk.memory));
      }
      else
      {
       retval = atoi(parseValue(chunk.memory));
      }
     }
    }

   free(chunk.memory);

   curl_easy_cleanup(curl);
  } // Endif (curl)

  return retval;
} // End Function pollServer()


//*****************************************************************************
// Infinite while loop to check current temperature, and compare it against
// our desired temperatures for the given time ranges.  Turn the heater on
// if it's too cold for the time of day, turn it off otherwise.
//*****************************************************************************
void runThermostat(void)
{
 char timestamp[6];
 time_t T;
 int currTemp;
 int currTime;
 int idx;
 int ret;

 heaterStatus = UNKNOWN; // Gotta start somewhere...

 while (1) // Run forever.
 {

  SAY("Check Programming from server.");

  // Get the current temperature from /tmp/temp
  currTemp = getCurrentTemperature();

  // Poll the server and see if we have new time/temp values
  ret = pollServer("MORNINGSTART", 1);
  if ((ret > 0) && (morningStart != ret))
  {
   SAY("New Morning Start time from server [%d]", ret);
   morningStart = ret;
  }

  ret = pollServer("AFTERNOONSTART", 1);
  if ((ret > 0) && (afternoonStart != ret))
  {
   SAY("New Afternoon Start time from server [%d]", ret);
   afternoonStart = ret;
  }

  ret = pollServer("NIGHTSTART", 1);
  if ((ret > 0) && (nightStart != ret))
  {
   SAY("New Night Start time from server [%d]", ret);
   nightStart = ret;
  }

  ret = pollServer("MORNINGTEMP", 0);
  if ((ret > 0) && (morningTemp != ret))
  {
   SAY("New Morning temperature from server [%d]", ret);
   morningTemp = ret;
  }

  ret = pollServer("AFTERNOONTEMP", 0);
  if ((ret > 0) && (afternoonTemp != ret))
  {
   SAY("New Afternoon temperature from server [%d]", ret);
   afternoonTemp = ret;
  }

  ret = pollServer("NIGHTTEMP", 0);
  if ((ret > 0) && (nightTemp != ret))
  {
   SAY("New Night temperature from server [%d]", ret);
   nightTemp = ret;
  }

  //
  if (currTemp > 0)
  {
   T = time(NULL);
   struct tm tm = *localtime(&T);

   memset(timestamp, '\0', sizeof(timestamp));
   sprintf(timestamp, "%02d:%02d", tm.tm_hour, tm.tm_min);
   currTime = _convertTimeToInteger(timestamp);

   // Check if we're in the 'morning' time range
   if ((currTime >= morningStart) && (currTime < afternoonStart))
   {
    SAY("Time: %s (morning). Desired Temp [%d], Curr Temp [%d]", timestamp, morningTemp, currTemp);
    toggleHeater(((currTemp >= morningTemp) ? OFF:ON), currTemp);
   }
   // Or we're in the 'afternoon' time range
   else if ((currTime >= afternoonStart) && (currTime < nightStart))
   {
    SAY("Time: %s (afternoon). Desired Temp [%d], Curr Temp [%d]", timestamp, afternoonTemp, currTemp);
    toggleHeater(((currTemp >= afternoonTemp) ? OFF:ON), currTemp);
   }
   // Change in logic to allow transition through midnight for 'night' time
   else if ((currTime >= nightStart) || (currTime < morningStart))
   {
    SAY("Time: %s (night). Desired Temp [%d], Curr Temp [%d]", timestamp, nightTemp, currTemp);
    toggleHeater(((currTemp >= nightTemp) ? OFF:ON), currTemp);
   }

  } // Endif (currTemp > 0)
  else
  {
   SAY("Unable to get current temperature");
  }

  sleep(15); // Wait 15 seconds before checking again.
  // Thermostat file is updated every 5 seconds.  Should we be more in synch?

 } // End while (1)

} // End Function runThermostat()


//*****************************************************************************
//*****************************************************************************
int main(int argc, char* argv[])
{
 int x;
 int ret;
 int shouldDemonize = 1;

 // ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
 // PROCESS THE COMMAND LINE:
 // ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
  memset(configFile, '\0', sizeof(configFile));
  for (x = 1; x < argc; x++)
  {
   if ((strcmp(argv[x], "-s") == 0) || (strcmp(argv[x], "--shell") == 0))
   {
    shouldDemonize = 0;
   }
   else if ((strcmp(argv[x], "-h") == 0) || (strcmp(argv[x], "--help") == 0))
   {
    theAction = Action_HELP;
   }
   else if ((strcmp(argv[x], "-c") == 0) || (strcmp(argv[x], "--config") == 0))
   {
    if (argc >= (x+2)) // Remember, index of argv[0] is count 1.  If we hit "-c" at argv[1], we need to make sure argc is at least 3
    {
     strncpy(configFile, argv[x+1], CONFIG_FILENAME_LENGTH-1);
     x++; // skip over it in the loop so we don't re-process it
    }
   }

  } // End for (x to argc)

 // User requested help - show help screen and get out.
 if (theAction == Action_HELP)
 {
  //performHelp();
  SAY("This is all the help you'll get\n");
  return OK;
 }

 if (shouldDemonize == 1)
 {
  if ((ret = _demon_stuff()) <= 0)
  {
   // The actual fork() command returns > 0 if we're the parent.
   // and < 0 if there's an error.
   // _demon_stuff() recognizes this and returns 0 when we're the parent.
   // We don't want to log a zero return code from _demon_stuff()
   if (ret < 0) printf("Error attempting to demonize [%d]\n", ret);

   // We still want to exit if ret <= 0
   return ret;
  }
 }

 // Read the config file.  Get out if we have an error.
 if ((ret = readConfigFile()) < 0)
 {
  SAY("Error reading config file[%d]\n", ret);
  return ret;
 }

 // Everything checks out - start heating the room!
 runThermostat();

 // In theory, we should never reach this line of code.
 return OK;

} // End Function main()