#include <unistd.h>
#include <ctype.h>
#include <sys/wait.h>
#include <stdio.h>
#include <stdlib.h>
#include <termios.h>            //termios, TCSANOW, ECHO, ICANON
#include <string.h>
#include <stdbool.h>
#include <errno.h>
#include <regex.h>
const char * sysname = "seashell";

enum return_codes {
	SUCCESS = 0,
	EXIT = 1,
	UNKNOWN = 2,
};
struct command_t {
	char *name;
	bool background;
	bool auto_complete;
	int arg_count;
	char **args;
	char *redirects[3]; // in/out redirection
	struct command_t *next; // for piping
};
/**
 * Prints a command struct
 * @param struct command_t *
 */
void print_command(struct command_t * command)
{
	int i=0;
	printf("Command: <%s>\n", command->name);
	printf("\tIs Background: %s\n", command->background?"yes":"no");
	printf("\tNeeds Auto-complete: %s\n", command->auto_complete?"yes":"no");
	printf("\tRedirects:\n");
	for (i=0;i<3;i++)
		printf("\t\t%d: %s\n", i, command->redirects[i]?command->redirects[i]:"N/A");
	printf("\tArguments (%d):\n", command->arg_count);
	for (i=0;i<command->arg_count;++i)
		printf("\t\tArg %d: %s\n", i, command->args[i]);
	if (command->next)
	{
		printf("\tPiped to:\n");
		print_command(command->next);
	}


}
/**
 * Release allocated memory of a command
 * @param  command [description]
 * @return         [description]
 */
int free_command(struct command_t *command)
{
	if (command->arg_count)
	{
		for (int i=0; i<command->arg_count; ++i)
			free(command->args[i]);
		free(command->args);
	}
	for (int i=0;i<3;++i)
		if (command->redirects[i])
			free(command->redirects[i]);
	if (command->next)
	{
		free_command(command->next);
		command->next=NULL;
	}
	free(command->name);
	free(command);
	return 0;
}
/**
 * Show the command prompt
 * @return [description]
 */
int show_prompt()
{
	char cwd[1024], hostname[1024];
    gethostname(hostname, sizeof(hostname));
	getcwd(cwd, sizeof(cwd));
	printf("%s@%s:%s %s$ ", getenv("USER"), hostname, cwd, sysname);
	return 0;
}
/**
 * Parse a command string into a command struct
 * @param  buf     [description]
 * @param  command [description]
 * @return         0
 */
int parse_command(char *buf, struct command_t *command)
{
	const char *splitters=" \t"; // split at whitespace
	int index, len;
	len=strlen(buf);
	while (len>0 && strchr(splitters, buf[0])!=NULL) // trim left whitespace
	{
		buf++;
		len--;
	}
	while (len>0 && strchr(splitters, buf[len-1])!=NULL)
		buf[--len]=0; // trim right whitespace

	if (len>0 && buf[len-1]=='?') // auto-complete
		command->auto_complete=true;
	if (len>0 && buf[len-1]=='&') // background
		command->background=true;

	char *pch = strtok(buf, splitters);
	command->name=(char *)malloc(strlen(pch)+1);
	if (pch==NULL)
		command->name[0]=0;
	else
		strcpy(command->name, pch);

	command->args=(char **)malloc(sizeof(char *));

	int redirect_index;
	int arg_index=0;
	char temp_buf[1024], *arg;
	while (1)
	{
		// tokenize input on splitters
		pch = strtok(NULL, splitters);
		if (!pch) break;
		arg=temp_buf;
		strcpy(arg, pch);
		len=strlen(arg);

		if (len==0) continue; // empty arg, go for next
		while (len>0 && strchr(splitters, arg[0])!=NULL) // trim left whitespace
		{
			arg++;
			len--;
		}
		while (len>0 && strchr(splitters, arg[len-1])!=NULL) arg[--len]=0; // trim right whitespace
		if (len==0) continue; // empty arg, go for next

		// piping to another command
		if (strcmp(arg, "|")==0)
		{
			struct command_t *c=malloc(sizeof(struct command_t));
			int l=strlen(pch);
			pch[l]=splitters[0]; // restore strtok termination
			index=1;
			while (pch[index]==' ' || pch[index]=='\t') index++; // skip whitespaces

			parse_command(pch+index, c);
			pch[l]=0; // put back strtok termination
			command->next=c;
			continue;
		}

		// background process
		if (strcmp(arg, "&")==0)
			continue; // handled before

		// handle input redirection
		redirect_index=-1;
		if (arg[0]=='<')
			redirect_index=0;
		if (arg[0]=='>')
		{
			if (len>1 && arg[1]=='>')
			{
				redirect_index=2;
				arg++;
				len--;
			}
			else redirect_index=1;
		}
		if (redirect_index != -1)
		{
			command->redirects[redirect_index]=malloc(len);
			strcpy(command->redirects[redirect_index], arg+1);
			continue;
		}

		// normal arguments
		if (len>2 && ((arg[0]=='"' && arg[len-1]=='"')
			|| (arg[0]=='\'' && arg[len-1]=='\''))) // quote wrapped arg
		{
			arg[--len]=0;
			arg++;
		}
		command->args=(char **)realloc(command->args, sizeof(char *)*(arg_index+1));
		command->args[arg_index]=(char *)malloc(len+1);
		strcpy(command->args[arg_index++], arg);
	}
	command->arg_count=arg_index;
	return 0;
}
void prompt_backspace()
{
	putchar(8); // go back 1
	putchar(' '); // write empty over
	putchar(8); // go back 1 again
}
/**
 * Prompt a command from the user
 * @param  buf      [description]
 * @param  buf_size [description]
 * @return          [description]
 */
int prompt(struct command_t *command)
{
	int index=0;
	char c;
	char buf[4096];
	static char oldbuf[4096];

    // tcgetattr gets the parameters of the current terminal
    // STDIN_FILENO will tell tcgetattr that it should write the settings
    // of stdin to oldt
    static struct termios backup_termios, new_termios;
    tcgetattr(STDIN_FILENO, &backup_termios);
    new_termios = backup_termios;
    // ICANON normally takes care that one line at a time will be processed
    // that means it will return if it sees a "\n" or an EOF or an EOL
    new_termios.c_lflag &= ~(ICANON | ECHO); // Also disable automatic echo. We manually echo each char.
    // Those new settings will be set to STDIN
    // TCSANOW tells tcsetattr to change attributes immediately.
    tcsetattr(STDIN_FILENO, TCSANOW, &new_termios);


    //FIXME: backspace is applied before printing chars
	show_prompt();
	int multicode_state=0;
	buf[0]=0;
  	while (1)
  	{
		c=getchar();
		// printf("Keycode: %u\n", c); // DEBUG: uncomment for debugging

		if (c==9) // handle tab
		{
			buf[index++]='?'; // autocomplete
			break;
		}

		if (c==127) // handle backspace
		{
			if (index>0)
			{
				prompt_backspace();
				index--;
			}
			continue;
		}
		if (c==27 && multicode_state==0) // handle multi-code keys
		{
			multicode_state=1;
			continue;
		}
		if (c==91 && multicode_state==1)
		{
			multicode_state=2;
			continue;
		}
		if (c==65 && multicode_state==2) // up arrow
		{
			int i;
			while (index>0)
			{
				prompt_backspace();
				index--;
			}
			for (i=0;oldbuf[i];++i)
			{
				putchar(oldbuf[i]);
				buf[i]=oldbuf[i];
			}
			index=i;
			continue;
		}
		else
			multicode_state=0;

		putchar(c); // echo the character
		buf[index++]=c;
		if (index>=sizeof(buf)-1) break;
		if (c=='\n') // enter key
			break;
		if (c==4) // Ctrl+D
			return EXIT;
  	}
  	if (index>0 && buf[index-1]=='\n') // trim newline from the end
  		index--;
  	buf[index++]=0; // null terminate string

  	strcpy(oldbuf, buf);

  	parse_command(buf, command);

  	// print_command(command); // DEBUG: uncomment for debugging

    // restore the old settings
    tcsetattr(STDIN_FILENO, TCSANOW, &backup_termios);
  	return SUCCESS;
}
int process_command(struct command_t *command);
int time_is_valid(char *string);
int main()
{
	while (1)
	{
		struct command_t *command=malloc(sizeof(struct command_t));
		memset(command, 0, sizeof(struct command_t)); // set all bytes to 0

		int code;
		code = prompt(command);
		if (code==EXIT) break;

		code = process_command(command);
		if (code==EXIT) break;

		free_command(command);
	}

	printf("\n");
	return 0;
}

//Regex and pattern matching for part4
int time_is_valid(char *string) {
	/*
	regex_t regex;
	int return_val;
	return_val = regcomp(&regex,"([0-1]\d|2[0-3]).[0-5]\d",0);
	if(return_val){
		printf("%s\n","Couldn't compile regex." );
	}

	return_val = regexec(&regex,string,0,NULL,0);
	printf("%d\n", return_val);*/
  return 0;
}

int process_command(struct command_t *command)
{
	int r;
	if (strcmp(command->name, "")==0) return SUCCESS;

	if (strcmp(command->name, "exit")==0)
		return EXIT;

	if (strcmp(command->name, "cd")==0)
	{
		if (command->arg_count > 0)
		{
			r=chdir(command->args[0]);
			if (r==-1)
				printf("-%s: %s: %s\n", sysname, command->name, strerror(errno));
			return SUCCESS;
		}
	}

	//Part2 shortdir
	if (strcmp(command->name, "shortdir")==0){
		FILE *file;
		//create the database file.
		char* db = "/home/koray/db.txt";
		file = fopen(db, "ab+");
		int LINE_LEN = 60;
		char line[LINE_LEN];

		if (strcmp((command->args)[0], "set")==0){
			const char *name = (command->args)[1];
			char cwd[100];
			if (getcwd(cwd, sizeof(cwd)) != NULL) {
				// print new pair in the database file
				fprintf(file, "%s = %s\n", name, cwd);
				fclose(file);
			}
			return SUCCESS;
    	}else if (strcmp((command->args)[0], "jump")==0){
			while( fgets(line, LINE_LEN, file) != NULL ) {
				char name[60];
				strcpy(name, (command->args)[1]);
				strcat(name," = ");
				if(strstr(line, name) != NULL) {
					//parse the databse file and find the queried short name.
					char * token = strtok(line, " = ");
					token = strtok(NULL, " = ");
					token[strcspn(token, "\n")] = 0;
					const char* name = token;
					//change the directory.
					chdir(name);
				}
			}
    	}else if (strcmp((command->args)[0], "del")==0){
			FILE *fileTemp;
			fileTemp = fopen("temp.txt", "w");
			while( fgets(line, LINE_LEN, file) != NULL ) {
				char name[60];
				strcpy(name, (command->args)[1]);
				strcat(name," = ");
				//check if the line has the the word to be deleted.
				if(strstr(line, name) == NULL) {
					//print the line to the temporary file
					fprintf(fileTemp, "%s", line);
					printf("%s",line);
				}
			}
			fclose(fileTemp);
			remove(db);
			rename("temp.txt", db);
    	}else if (strcmp((command->args)[0], "clear")==0){
			//create a new file with the same name
			fclose(fopen(db, "w"));
    	}else if (strcmp((command->args)[0], "list")==0){
			while( fgets(line, LINE_LEN, file) != NULL ) {
				//print each line one by one.
				printf("%s",line);
			}
    	}
		return SUCCESS;
	}

	//Part3 highlight
	if (strcmp(command->name, "highlight")==0){
		char* color = "\x1b[31m";
		char* reset = "\x1b[0m";
		//change the color code based on the argument given by the user.
		if (strcmp((command->args)[1], "b")==0){
			color = "\x1b[34m";
		}else if (strcmp((command->args)[1], "g")==0)
		 	color = "\x1b[32m";

		int LINE_LEN = 200;
		char line[LINE_LEN];
		FILE *file;
		if((file = fopen((command->args)[2], "r")) == NULL) {
			printf("Could not open input file\n");
			return UNKNOWN;
		}
		//get each line of the file line by line
		while( fgets(line, LINE_LEN, file) != NULL ) {
			char line2[LINE_LEN];
			strcpy(line2,line);
			for(int i = 0; line2[i]; i++){
                line2[i] = tolower(line2[i]);
            }
			//check if the word occurs in that line
			if(strstr(line2, (command->args)[0]) != NULL) {
				char * token = strtok(line, " ");
				while( token != NULL ) {
					token[strcspn(token, "\n")] = 0;
					char token2[LINE_LEN];
					strcpy(token2,token);
					//lower each char of the world because comparison is not case sensitive.
					for(int i = 0; token2[i]; i++){
						token2[i] = tolower(token2[i]);
					}
					char dot[60];
					strcpy(dot, (command->args)[0]);
					strcat(dot,".");
					//check if the token is the word to change 
					if(strcmp(token2, (command->args)[0])==0 || strcmp(token2, dot)==0){
						//print the word with the color code before and reset code after it.
						printf("%s%s%s ",color,token,reset);
					}else {
						//print the word without changing the color
						printf("%s ",token);
					}
					token = strtok(NULL, " ");
				}
				printf("\n");
			}
		}
		return SUCCESS;
	}

	//Part4 goodMorning
	if (strcmp(command->name, "goodMorning")==0){

			int time[1];
			char * token = strtok((command->args)[0], ".");
			char * path = (command->args)[1];
			int i = 0;
			//dissect first variable, time, into hours and minutes
			while(token!=NULL){
					time[i] = atoi(token);
					token = strtok(NULL,".");
					i++;
			}
			//open a cronfile and write a cron job with the given parameters that will play the sound on rhythmbox
			FILE *fptr;
			fptr = fopen("/home/koray/cronfile.txt","w");
			if(fptr !=NULL){
					fprintf(fptr, "%d %d * * * DISPLAY=:0.0 /usr/bin/rhythmbox-client --play-uri=\"%s\"\n",time[1], time[0], path);
			}else{
				printf("%s\n","Cannot open the file specified." );
				return UNKNOWN;
			}
			fclose(fptr);
			//assign the cronfile.txt as the designated cron file 
			execlp("crontab", "crontab","/home/koray/cronfile.txt",NULL);
			return SUCCESS;
	}

	//Part5 kdiff
	if (strcmp(command->name, "kdiff")==0){
			FILE *fptr1, *fptr2;
			char tmp1, tmp2;
			int LINE_LEN = 30;
			char fptr1_line[LINE_LEN];
			char fptr2_line[LINE_LEN];
		//comparison by line	
		if (strcmp((command->args)[0], "-a")==0){
			fptr1 = fopen((command->args)[1],"r");
			fptr2 = fopen((command->args)[2],"r");

			if(fptr2 ==NULL || fptr1 ==NULL){
				printf("%s\n","Cannot open the file specified." );
				exit(1);
			}else{
			//compares two files line by line while counting the total lines and total difference
				int line_count = 0;
				int total_diff = 0;
				while(fgets(fptr1_line, LINE_LEN, fptr1) != NULL
				 	&& fgets(fptr2_line, LINE_LEN, fptr2) != NULL) {
						line_count++;

						if(strcmp(fptr1_line, fptr2_line)!=0){
							printf("%s:Line %d: %s\n",(command->args)[1], line_count, fptr1_line);
							printf("%s:Line %d: %s\n",(command->args)[2], line_count, fptr2_line);
							total_diff++;
						}
				}
					if(total_diff == 0)printf("The two files are identical.\n");
					else			  printf("%d different lines found.\n",total_diff);

			}
			//comparison by bytes
		}else if(strcmp((command->args)[0], "-b")==0){
			printf("1=%s -- 2=%s\n",(command->args)[1], (command->args)[2]);
			fptr1 = fopen((command->args)[1],"rb");
			fptr2 = fopen((command->args)[2],"rb");

			if(fptr2 ==NULL || fptr1 ==NULL){
				printf("%s\n","Cannot open the file specified." );
				return UNKNOWN;
			}else{
			//compares file size difference
				fseek(fptr1, 0L, SEEK_END);
				long int fptr1_size = ftell(fptr1);
				fseek(fptr2, 0L, SEEK_END);
				long int fptr2_size = ftell(fptr2);

				if (fptr1_size != fptr2_size) {
				        printf("File sizes differ, %ld vs. %ld\n",fptr1_size,fptr2_size);
				    }
				//compares the files byte by byte and increments xyz accordingly
				fseek(fptr1, 0L, SEEK_SET);
				fseek(fptr2, 0L, SEEK_SET);
				int xyz = 0;
			 	for (int i=0;i<fptr1_size;i++) {
					fread(&tmp1, 1, 1, fptr1);
					fread(&tmp2, 1, 1, fptr2);
				    if (tmp1 != tmp2) {
				    	xyz++;
				    }
				}
				//prints the number of different bytes
				if(xyz != 0)	printf("The two files are different in %d bytes.\n",xyz);
				else			printf("The two files are identical.\n");
			}
		}

		fclose(fptr1);
		fclose(fptr2);
		return SUCCESS;
	}

	//Part6 changeWord
	if (strcmp(command->name, "changeWord")==0){
			int LINE_LEN = 200;
			char line[LINE_LEN];
			FILE *file;
			char* fileName = (command->args)[2];
			if((file = fopen(fileName, "r")) == NULL) {
				printf("Could not open input file\n");
				return UNKNOWN;
			}
			FILE *fileTemp;
			fileTemp = fopen("temp.txt", "w");
			//get each line of the file line by line
			while( fgets(line, LINE_LEN, file) != NULL ) {
				char line2[LINE_LEN];
				strcpy(line2,line);
				for(int i = 0; line2[i]; i++){
					line2[i] = tolower(line2[i]);
				}
				//check if the word occurs in that line
				if(strstr(line2, (command->args)[0]) == NULL) {
					fprintf(fileTemp, "%s", line);
				}else{
					char * token = strtok(line, " ");
					while( token != NULL ) {
						token[strcspn(token, "\n")] = 0;
						char token2[LINE_LEN];
						strcpy(token2,token);
						//lower each char of the world because comparison is not case sensitive.
						for(int i = 0; token2[i]; i++){
							token2[i] = tolower(token2[i]);
						}
						char dot[60];
						strcpy(dot, (command->args)[0]);
						strcat(dot,".");
						//check if the token is the word to change 
						if (strcmp(token2, (command->args)[0])==0){
							//replace the word with the other word (second argunment) or replace it with redacted if the second argument is -del
							if(strcmp((command->args)[1], "-del")==0)fprintf(fileTemp, "[redacted] ");
							else fprintf(fileTemp, "%s ", (command->args)[1]);
						//check if the token is the word to change with a dot
						}else if(strcmp(token2, dot)==0){
							if(strcmp((command->args)[1], "-del")==0) fprintf(fileTemp, "[redacted]. ");
							else fprintf(fileTemp, "%s. ", (command->args)[1]);
						}else{
							fprintf(fileTemp, "%s ", token);
						}
						token = strtok(NULL, " ");
					}
					fprintf(fileTemp, "\n");
				}
			}
			fclose(fileTemp);
			remove(fileName);
			rename("temp.txt", fileName);
			return SUCCESS;
		}

	pid_t pid=fork();
	if (pid==0) // child
	{
		/// This shows how to do exec with environ (but is not available on MacOs)
	    // extern char** environ; // environment variables
		// execvpe(command->name, command->args, environ); // exec+args+path+environ

		/// This shows how to do exec with auto-path resolve
		// add a NULL argument to the end of args, and the name to the beginning
		// as required by exec

		// increase args size by 2
		command->args=(char **)realloc(
			command->args, sizeof(char *)*(command->arg_count+=2));

		// shift everything forward by 1
		for (int i=command->arg_count-2;i>0;--i)
			command->args[i]=command->args[i-1];

		// set args[0] as a copy of name
		command->args[0]=strdup(command->name);
		// set args[arg_count-1] (last) to NULL
		command->args[command->arg_count-1]=NULL;

		//execvp(command->name, command->args); // exec+args+path

		//Part1
		//get the environment variable
		char* paths = getenv("PATH");
		//parse the path variables
		char * token = strtok(paths, ":");

		while( token != NULL ) {
			char path[30];
			strcpy(path, token);
			strcat(path,"/");
			strcat(path,command->name);
			//execute the command when the correct path variable is found.
			execv(path, command->args);
      		token = strtok(NULL, ":");
   		}
		exit(0);

	}
	else
	{
		if (!command->background)
			wait(0); // wait for child process to finish
		return SUCCESS;
	}


	// TODO: your implementation here

	printf("-%s: %s: command not found\n", sysname, command->name);
	return UNKNOWN;
}
