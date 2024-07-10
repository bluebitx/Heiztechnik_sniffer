/*
 * Click nbfs://nbhost/SystemFileSystem/Templates/Licenses/license-default.txt to change this license
 * Click nbfs://nbhost/SystemFileSystem/Templates/cFiles/main.c to edit this template
 */

/* 
 * File:   main.c
 * Author: Bartosz 'BlueBit' Jaszul <bjaszul@gmail.com>
 *
 * Created on 16 listopada 2023, 20:57
 */

#include <stdio.h>
#include <unistd.h>			//Used for UART
#include <fcntl.h>			//Used for UART
#include <termios.h>		//Used for UART
#include <stdlib.h>
#include <mysql.h>                      //used for mysql libary libmariadb-dev-compat
#include <time.h>                   //time
#include <string.h>
#include <stdbool.h>  //bool
#include <modbus.h>
#include <errno.h>

#define Praca 0x01
#define Tryb_reczny 0x02
#define Alarm 0x04
#define Rozpalanie 0x08
#define Tajne 0x10  
#define Testowy 0x20
#define LetniTryb 0x50

typedef unsigned char uint8_t;

struct Settings{
    uint8_t Automatic_water_heat;                                               //Automaticly turn on stove (in summer) to heat water to desired temperature (0-off,else desired temp 40-70 ),
    uint8_t Incrase_time_betwen_fireup;                                         //When stove goes sleep (becouse everything is heated and nothing takes power) add extra sleep time :) (0-off, else extra time in minutes 1-250 )
    uint8_t Cold_night;                                                         //Turn off stove (if it is in sleep mode) before restart, if it after 23:00 and automaticly start at 5:00, if 0-off, else MSB hour of turn on function, LSB hour on start fireplace      
}__attribute__((__packed__));

union {
    struct Settings DB;
    char RAW[sizeof(struct Settings)];
}Sets;
    
struct StoveParameters{
    int8_t Temperatura_pieca;
    int8_t Temperatura_powrotu;
    int8_t Temperatura_CWU;
    int8_t Temperatura_palnika;
    int8_t Temperatura_zewnetrzna;
    int8_t Temperatura_zaworu;
    int8_t Poziom_paliwa;
    int8_t Plomien;
    unsigned char Moc;
    int8_t Wentylator;
    unsigned char Status_1;
    unsigned char Status_2;
    unsigned char Status_3;
};

struct power{
    float U1;
    float U2;
    float U3;
    float U12;
    float U23;
    float U31;
    float I1;
    float I2;
    float I3;
    float P1;
    float P2;
    float P3;
    float Freq;
    float Energy;
}energyMeter;


const char *lookuptable[] = { "Uruchom" , "Wylacz" , "Kasuj_bledy" , "Ustaw100" , "WlaczCO" , "WylaczCO" , "WylaczPR" , "WylaczTrybZimowy" , "WylaczTrybLetni" } ;
enum com { run = 0 , stop , errase_errors, set_fuel_to_100 , turnON_CO_pump , turnOFF_CO_pump , turn_off_before_next_start ,WinterMode, SummerMode, NumberOfTypes};

uint8_t Turn_off_before_next_start=0;

bool  DataType4_received = false ;
bool  DataType5_received = false ;
unsigned char Rx_flag_lineready = 1 ;
unsigned char Rx_bufindex = 0 ;
#define Rx_bufindex_MAX 254
unsigned char Rx_buf[Rx_bufindex_MAX];
char Tx_buf[100];
int Stove_uart_filestream = -1 ; 
FILE *fp;
MYSQL *mysql_con;
modbus_t *pm;
bool PMconnected=false;
unsigned int DATABASEerror=0;

struct StoveParameters stove_data = {0};
struct StoveParameters old_stove_data = {0};

bool connect_to_powermeter(void);   //Open uart connected to PowerMeter
char Open_stove_uart();             //Open uart connected to stove
void Receive_data_from_stoveUART(); //Read data from stoveUART        
void Process_datagram();            //Parse stove data from readed UART data
void Save_data_in_database();       //Store stove data to database (for charts reason)
void CheckExternalWebControl();        //reading direct commands from database
void RunStoveCommand(uint8_t command); //Send command to Stove :)
unsigned char Send2Stove(unsigned char * data,unsigned char len);   //Sending something to Stove
void Turn_off_before_start_function();      //turn off before start function 
unsigned char put_into_database(char opis[30],int8_t value); //put current values in database
bool get_data_from_powermeter(void);    //read data from powerMeter via modbus protocol
void Auto_turn_on_at_defined_hour(void); //Auto run stove at specific hour
unsigned char  put_into_databasef(char opis[30],float value); //Put current value in database
char read_from_database(char opis[30]); //Read current value in database
void  put_energy_data_into_databasef(struct power M); //put PowerMeter data to database
void Auto_turn_off_in_summer_time();// auto off when we are in summer time and water is heated 

// MAIN FUNCTION !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
int main(int argc, char** argv) {
    printf("Heiztechnik reader v2.04   \r\n");
    
    //Init connection with mysql database (mariadb)
    mysql_con = mysql_init(NULL);
    
    if (mysql_con == NULL){
        printf("Database error - Init fail\r\n");
        return (EXIT_SUCCESS);
    }
    
    if (mysql_real_connect(mysql_con,"192.168.5.1","piec2","OguqDaFwEnXg8Hrd","piec2", 0, NULL,0)==NULL){
        printf("Database error - Connection fail\r\n");
        mysql_close(mysql_con); 
        return (EXIT_SUCCESS);
    }else{
        printf("Database - connected\r\n");
    }
    // connection to database
   
    //Init Uart
    Open_stove_uart();
    if (connect_to_powermeter())PMconnected=true;    
    
    Turn_off_before_next_start = read_from_database("TurnOffBeforeNextRun");
    printf("Turn off beffore next run - status %s\r\n",Turn_off_before_next_start?"ON":"OFF");
    while (1) { //main loop
        
        //stove functions 
        Rx_flag_lineready=0;
        Receive_data_from_stoveUART();  //reading data from database
        
        if (Rx_flag_lineready) Process_datagram(); // print data on console and save current values in database
        
        //copy data from current data to old data on the start 
        if (old_stove_data.Poziom_paliwa==0) old_stove_data = stove_data;
        
        //save data to database (for charts) - after each 60 seconds
        Save_data_in_database();
        
        //external control from website 
        CheckExternalWebControl();
        
        //function Turn_off_before_next_start 
        if (Turn_off_before_next_start)Turn_off_before_start_function();
        
        //auto off when stove is in summer mode and water temp is high
        Auto_turn_off_in_summer_time();
        
        Auto_turn_on_at_defined_hour();
        
        //power meter functions 
        if (PMconnected)if (get_data_from_powermeter()==0)printf("Blad odczytu z energyMetera\r\n");
        
        usleep(500);
        
        if (DATABASEerror>100)return -1;
    }
    
    
    
    return (EXIT_SUCCESS);
}
// MAIN FUNCTION !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!

void Auto_turn_off_in_summer_time(){
    if ( ( stove_data.Status_1 & Praca )  && ( stove_data.Status_1 & LetniTryb ) ){ //if stove is ON and summer mode
        if ( stove_data.Temperatura_CWU >= 55 ){
            if (Turn_off_before_next_start==0)Turn_off_before_next_start = 1;
        }
    }
}

void Auto_turn_on_at_defined_hour(){
    static unsigned int last_check;
    uint8_t hour;
    
    if ( (stove_data.Status_1 & 0x01)==0){ //if stove is switch off 
        if (time(NULL)>last_check+60){ // check every 10 sec.
            last_check=time(NULL);
            hour = read_from_database("TurnOnAtDefinedHour");
            printf("Auto turn on at defined hour - defined hour is %d\r\n",hour);
            if ( hour != 24 ){
                struct tm * timeinfo;
                time_t rawtime;
                time (&rawtime);
                timeinfo = localtime(&rawtime);
                printf("Current hour is %d \r\n",timeinfo->tm_hour);
                if ( hour == timeinfo->tm_hour )RunStoveCommand(run);
            }
        }
    }
    
    
}

bool get_data_from_powermeter(void){
    static unsigned int last_check_settings;
    
    if (time(NULL)>last_check_settings+5){ // check every 10 sec.
        last_check_settings=time(NULL);
        unsigned short zwrot[20];
        //napiecia
        if (modbus_read_input_registers(pm,20509,12,zwrot)>0){
            energyMeter.U1=((float)(zwrot[0]<<16|zwrot[1])/1000);
            energyMeter.U2=((float)(zwrot[2]<<16|zwrot[3])/1000);
            energyMeter.U3=((float)(zwrot[4]<<16|zwrot[5])/1000);   
            energyMeter.U12 = ((float)(zwrot[6]<<16|zwrot[7])/1000);
            energyMeter.U23 = ((float)(zwrot[8]<<16|zwrot[9])/1000);
            energyMeter.U31 = ((float)(zwrot[10]<<16|zwrot[11])/1000);
        }else{
            modbus_free(pm);
            connect_to_powermeter();
            return 0;
        }
        //prady
        if (modbus_read_input_registers(pm,20480,6,zwrot)>0){
            energyMeter.I1 = ((float)(zwrot[0]<<16|zwrot[1])/1000);
            energyMeter.I2 = ((float)(zwrot[2]<<16|zwrot[3])/1000);
            energyMeter.I3 = ((float)(zwrot[4]<<16|zwrot[5])/1000);
        }else{
            modbus_free(pm);
            connect_to_powermeter();
            return 0;
        }

        //moc
        if (modbus_read_input_registers(pm,20550,13,zwrot)>0){
            energyMeter.P1=((float)(zwrot[1]<<16|zwrot[2])/100);
            energyMeter.P2=((float)(zwrot[3]<<16|zwrot[4])/100);
            energyMeter.P3=((float)(zwrot[5]<<16|zwrot[6])/100);
        }else{
            modbus_free(pm);
            connect_to_powermeter();
            return 0;
        }

        // czestotliwosc
        if (modbus_read_input_registers(pm,20537,5,zwrot)>0){
            energyMeter.Freq=(((float)zwrot[0])*0.01);
        }else{
            modbus_free(pm);
            connect_to_powermeter();
            return 0;
        }
        // energyMeter
        if (modbus_read_input_registers(pm,20592,4,zwrot)>0){
            energyMeter.Energy=((float)(zwrot[0]<<16|zwrot[1])/100);
        }else{
            modbus_free(pm);
            connect_to_powermeter();
            return 0;
        }
        
        //==U1==|==U2==|==U3==|===I1==|===I2==|===I3==|==P1==|==P2==|==P3==|=Psum=|=Freq=|=Power used=|
        // 214V | 212V | 218V | 0.62A | 1.80A | 0.59A |  93W | 249W |  70W | 412W | 50.01Hz| 10736 kWh |

        printf("==U1==|==U2==|==U3==|===I1==|===I2==|===I3==|==P1==|==P2==|==P3==|=Psum=|==Freq==|=Power used=|\r\n");
        printf(" %3.0fV | %3.0fV | %3.0fV | %3.2fA | %3.2fA | %3.2fA | %3.0fW | %3.0fW | %3.0fW | %3.0fW | %2.2fHz| %6.0f kWh |\r\n\r\n",energyMeter.U1,energyMeter.U2,energyMeter.U3,energyMeter.I1,energyMeter.I2,energyMeter.I3,energyMeter.P1,energyMeter.P2,energyMeter.P3,(energyMeter.P1+energyMeter.P2+energyMeter.P3),energyMeter.Freq,energyMeter.Energy);
        
        put_energy_data_into_databasef(energyMeter);
        
    }
    
    

    return 1;
}


void Turn_off_before_start_function(){
    //save status to data base 
    
    if (DataType4_received & DataType5_received){
        static time_t Retry=0;

        if ( stove_data.Status_1 & Praca ){
            if ( time(NULL) > Retry ){
                if ( stove_data.Status_1 & Rozpalanie ){
                    printf("Ignition detected - turning off stove\r\n");
                    RunStoveCommand(stop);
                    Retry = time(NULL)+10; 
                }
            }
        }else{
            printf("Stove is turned off - so i clear job flag \r\n");
            put_into_database("TurnOffBeforeNextRun",0);
            Turn_off_before_next_start = 0;
            Retry = 0;
        }
    }
    
    return;
}


bool connect_to_powermeter(void){
    if (PMconnected==false){
        pm = modbus_new_rtu("/dev/ttyUSBlicznik", 19200 , 'N', 8, 1);
        if (pm == NULL) {
            fprintf(stderr, "Unable to create the libmodbus context\n");
            return 0;
        }
        modbus_set_slave(pm, 1);

        if (modbus_connect(pm) == -1) {
            fprintf(stderr, "Connection to power meter failed: %s\n", modbus_strerror(errno));
            modbus_free(pm);
            return 0;
        }
    }
    return 1;
}





void RunStoveCommand(uint8_t command){
    switch (command){
        case run :
            printf("\r\n\r\n!!!!!!!!!!!!!!!!!!!!!!!!!!WebCommand -> Run stove\r\n\r\n");
            Tx_buf[0]=0x01;Tx_buf[1]=0x02;Tx_buf[2]=0x01;Tx_buf[3]=0x00;Tx_buf[4]=0x00;Tx_buf[5]=0x00;Send2Stove(Tx_buf,5); //uruchominie
        break;

        case WinterMode:
            printf("\r\n\r\n!!!!!!!!!!!!!!!!!!!!!!!!!!WebCommand -> Winter mode\r\n");
            Tx_buf[0]=0x01;Tx_buf[1]=0x02;Tx_buf[2]=0x03;Tx_buf[3]=0x09;Tx_buf[4]=0x06;Tx_buf[5]=0x2E;Tx_buf[6]=0x00;Tx_buf[7]=0x01;Tx_buf[8]=0x7D;Tx_buf[9]=0x68;Send2Stove(Tx_buf,9); //Turn on Winter mode
        break;
        
        case SummerMode:
            printf("\r\n\r\n!!!!!!!!!!!!!!!!!!!!!!!!!!WebCommand -> Summer mode\r\n");
            Tx_buf[0]=0x01;Tx_buf[1]=0x02;Tx_buf[2]=0x03;Tx_buf[3]=0x09;Tx_buf[4]=0x06;Tx_buf[5]=0x2E;Tx_buf[6]=0x00;Tx_buf[7]=0x02;Tx_buf[8]=0x3D;Tx_buf[9]=0x69;Send2Stove(Tx_buf,9); //Turn on Winter mode
        break;
        
        case stop :
            printf("\r\n\r\n!!!!!!!!!!!!!!!!!!!!!!!!!!WebCommand -> Stop stove\r\n");
            Tx_buf[0]=0x01;Tx_buf[1]=0x02;Tx_buf[2]=0x01;Tx_buf[3]=0x01;Tx_buf[4]=0x00;Tx_buf[5]=0x00;Send2Stove(Tx_buf,5); //wygaszenie
        break;

        case errase_errors:
            printf("\r\n\r\n!!!!!!!!!!!!!!!!!!!!!!!!!!WebCommand -> Errase all faults\r\n");
            Tx_buf[0]=0x01;Tx_buf[1]=0x02;Tx_buf[2]=0x00;Tx_buf[3]=0x0B;Tx_buf[4]=0x00;Send2Stove(Tx_buf,4); //kasowanie listy bledow ?
        break;

        case set_fuel_to_100:
            printf("\r\n\r\n!!!!!!!!!!!!!!!!!!!!!!!!!!WebCommand -> Set 100 per. pellets\r\n");
            Tx_buf[0]=0x01;Tx_buf[1]=0x02;Tx_buf[2]=0x03;Tx_buf[3]=0x09;Tx_buf[4]=0x06;Tx_buf[5]=0x22;Tx_buf[6]=0x00;Tx_buf[7]=0x02;Send2Stove(Tx_buf,7); //ustawianie 100% peletu
        break;

        case turnON_CO_pump:
            printf("\r\n\r\n!!!!!!!!!!!!!!!!!!!!!!!!!!WebCommand -> Turn on CO pump\r\n");
            Tx_buf[0]=0x01;Tx_buf[1]=0x02;Tx_buf[2]=0x01;Tx_buf[3]=0x00;Tx_buf[4]=0x00;Tx_buf[5]=0x0B;Send2Stove(Tx_buf,5); //wlaczenie pompy co
        break;    

        case turnOFF_CO_pump:
            printf("\r\n\r\n!!!!!!!!!!!!!!!!!!!!!!!!!!WebCommand -> Turn off CO pump\r\n");
            Tx_buf[0]=0x01;Tx_buf[1]=0x02;Tx_buf[2]=0x01;Tx_buf[3]=0x01;Tx_buf[4]=0x00;Tx_buf[5]=0x0B;Send2Stove(Tx_buf,5); //wylaczenie pompy co                    
        break;    

        case turn_off_before_next_start:
            Turn_off_before_next_start=1;
        break;
    }
    
    return;
}

void CheckExternalWebControl(){
    char query[500];
    unsigned char i=0;
    unsigned char Tx_buf[255];
    static unsigned int last_check_settings;
    
    if (time(NULL)>last_check_settings+5){ // check every 10 sec.
        printf("Checing webControl\r\n");
        last_check_settings=time(NULL);
        struct tm * timeinfo;
        uint8_t old_Turn_off_before_next_start = Turn_off_before_next_start;
        time_t rawtime;
        time (&rawtime);
        timeinfo = localtime(&rawtime);
        char buffer[80];
        strftime(buffer,sizeof(buffer),"%Y-%m-%d %H:%M:%S",timeinfo);
        MYSQL_ROW row;
        // checking that exist command with realization timestamp is equal to 00:00:00
        snprintf(query,sizeof(query),"SELECT * FROM Sterowanie_piecem WHERE `timestamp_wykonania`='0000-00-00 00:00:00'");
        if (mysql_query(mysql_con, query)){printf("Blad nie ma takiej wartosci\r\n");return;}
        MYSQL_RES *result = mysql_store_result(mysql_con);
        if (mysql_num_rows(result)){
            while ((row = mysql_fetch_row(result))){
                for (int i = 0 ; i < (sizeof(lookuptable)/sizeof(char *)) ; i++ ){
                    if (strcmp(row[1] , lookuptable[i] ) == 0 ){ 
                        RunStoveCommand(i);
                        snprintf(query,sizeof(query),"UPDATE `Sterowanie_piecem` SET `timestamp_wykonania`='%s' WHERE `lp`='%s';",buffer,row[0]);            
                        if (mysql_query(mysql_con,query)){printf("2Blad wprowadzania do bazy parametrów pracy pieca\r\n");}       
                    }
                    
                }
            }
            mysql_free_result(result);
            
            if (Turn_off_before_next_start != old_Turn_off_before_next_start)put_into_database("TurnOffBeforeNextRun",1);
                
            return ;
        }else{
            mysql_free_result(result);
        }
    return ;
    }
}
/*
unsigned char Save_data_(struct StoveParameters parametry){
    char query[500];
    unsigned int timestamp=time(NULL);
    
    //sprawdzamy czy jest juz taki timeout w bazie danych
    snprintf(query,sizeof(query),"SELECT * FROM Praca_pieca WHERE `Timestamp`='%u'",timestamp);
    if (mysql_query(mysql_con, query)){printf("Blad nie ma takiej wartosci\r\n");return 0;}
    
    MYSQL_RES *result = mysql_store_result(mysql_con);
    if (mysql_num_rows(result)){
        mysql_free_result(result);
        return 1;
    }else{
        mysql_free_result(result);
        //nie ma takiego wpisu w bazie danych // wpisujemy
        snprintf(query,sizeof(query),"INSERT INTO Praca_pieca(`Temperatura_pieca`,`Temperatura_powrotu`,`Temperatura_CWU`,`Temperatura_palnika`,`Temperatura_zewnetrzna`,`Temperatura_zaworu`,`Poziom_paliwa`,`Moc`,`Wentylator`,`Status_1`,`Status_2`,`Status_3`,`Plomien`) VALUES ('%d','%d','%d','%d','%d','%d','%d','%u','%d','%d','%d','%d','%d')",parametry.Temperatura_pieca,parametry.Temperatura_powrotu,parametry.Temperatura_CWU,parametry.Temperatura_palnika,parametry.Temperatura_zewnetrzna,parametry.Temperatura_zaworu,parametry.Poziom_paliwa,parametry.Moc,parametry.Wentylator,parametry.Status_1,parametry.Status_2,parametry.Status_3,parametry.Plomien);
        if (!mysql_query(mysql_con,query)){printf("1Blad wprowadzania do bazy parametrów pracy pieca\r\n");return 0;}    
    }
    
return 1;
}
 * */

//Save data to logging reason
void Save_data_in_database(){
    unsigned long current_time = (unsigned long)time(NULL);
    static unsigned int last_saving_timestamp = 0;
    if ( last_saving_timestamp == 0 ) last_saving_timestamp = time(NULL);
    
    if ( DataType4_received & DataType5_received & (( current_time - last_saving_timestamp ) >= 60 ) & ( current_time % 60 == 0 )){
        if ( stove_data.Poziom_paliwa == 0 ) old_stove_data = stove_data ;
        last_saving_timestamp = current_time ;
        printf("\r\n Saving data to database \r\n");
        char query[500];
        
        //Check database with the same timestamp
        snprintf(query,sizeof(query),"SELECT * FROM Praca_pieca WHERE `Timestamp`='%u'",current_time);
        if (mysql_query(mysql_con, query)){printf("Dubel - cancel\r\n");return;}
    
        MYSQL_RES *result = mysql_store_result(mysql_con);
        if (mysql_num_rows(result)){
            mysql_free_result(result);
            return ;
        }else{
            mysql_free_result(result);
            //nie ma takiego wpisu w bazie danych // wpisujemy
            snprintf(query,sizeof(query),"INSERT INTO Praca_pieca(`Temperatura_pieca`,`Temperatura_powrotu`,`Temperatura_CWU`,`Temperatura_palnika`,`Temperatura_zewnetrzna`,`Temperatura_zaworu`,`Poziom_paliwa`,`Moc`,`Wentylator`,`Status_1`,`Status_2`,`Status_3`,`Plomien`) VALUES ('%d','%d','%d','%d','%d','%d','%d','%u','%d','%d','%d','%d','%d')",stove_data.Temperatura_pieca,stove_data.Temperatura_powrotu,stove_data.Temperatura_CWU,stove_data.Temperatura_palnika,stove_data.Temperatura_zewnetrzna,stove_data.Temperatura_zaworu,stove_data.Poziom_paliwa,stove_data.Moc,stove_data.Wentylator,stove_data.Status_1,stove_data.Status_2,stove_data.Status_3,stove_data.Plomien);
            if (!mysql_query(mysql_con,query)){
                printf("1Blad wprowadzania do bazy parametrów pracy pieca\r\n");
                //DATABASEerror++;
                return ;
            }//else{
            //    DATABASEerror=0;
            //}    
        }
        
    }//if
}//void


//check that current parameter is in database
unsigned char exist_in_database(char opis[30]){
    char query[200];
    snprintf(query,sizeof(query),"SELECT * FROM aktualne_wartosci WHERE `nazwa`='%s'",opis);
    if (mysql_query(mysql_con, query)){printf("Blad nie ma takiej wartosci\r\n");return 0;}
    MYSQL_RES *result = mysql_store_result(mysql_con);
    if (mysql_num_rows(result)){
        mysql_free_result(result);
        return 1;
    }else{
        mysql_free_result(result);
        return 0;
    }
    
}

//put current energy data into database
void  put_energy_data_into_databasef(struct power M){
    char query[300];
    snprintf(query,sizeof(query),"INSERT INTO Licznik(`Rvoltage`,`Svoltage`,`Tvoltage`,`Rcurrent`,`Scurrent`,`Tcurrent`,`Rpower`,`Spower`,`Tpower`,`Frequency`,`TotalEnergy` )VALUES ('%.1f','%.1f','%.1f','%.1f','%.1f','%.1f','%.1f','%.1f','%.1f','%.2f','%.2f')",M.U1,M.U2,M.U3,M.I1,M.I2,M.I3,M.P1,M.P2,M.P3,M.Freq,M.Energy);
    if (mysql_query(mysql_con,query)){printf("ErBlad dodawania dancy do licznika\r\n %s \r\n",query);}    
    MYSQL_RES *result = mysql_store_result(mysql_con);
    mysql_free_result(result);
    
}
//put current calue in database (float)
unsigned char  put_into_databasef(char opis[30],float value){
    
    char query[200];
    if (exist_in_database(opis)){
         struct tm * timeinfo;
        time_t rawtime;
        time (&rawtime);
        timeinfo = localtime(&rawtime);
        char buffer[80];
        strftime(buffer,sizeof(buffer),"%Y-%m-%d %H:%M:%S",timeinfo);
        snprintf(query,sizeof(query),"UPDATE `aktualne_wartosci` SET `wartosc`='%.1f', `data_aktualizacji`='%s' WHERE `nazwa`='%s'",value,buffer,opis);
        if (mysql_query(mysql_con,query)){
            printf("Blad wykonania zmiany tej wartosci %s\r\n",opis);
            DATABASEerror++;
            return 0;
        }else{
            DATABASEerror=0;
        }
    }else{
        snprintf(query,sizeof(query),"INSERT INTO aktualne_wartosci(`nazwa`,`wartosc`) VALUES ('%s','%.1f')",opis,value);
        if (mysql_query(mysql_con,query)){
            printf("Blad wykonania dodania tej wartosci %s\r\n",opis);
            DATABASEerror++;
            return 0;
        }else{
            DATABASEerror=0;
        }
    }
    MYSQL_RES *result = mysql_store_result(mysql_con);
    mysql_free_result(result);
     
return 1;
}

char read_from_database(char opis[30]){
    char query[500];
    char ret=-1;
    MYSQL_ROW row;
    snprintf(query,sizeof(query),"SELECT * FROM aktualne_wartosci WHERE `nazwa`='%s'",opis);
    if (mysql_query(mysql_con, query)){return -1;}
        MYSQL_RES *result = mysql_store_result(mysql_con);
    if (mysql_num_rows(result)){
        while ((row = mysql_fetch_row(result))){
            ret = atoi(row[2]);
        }
        mysql_free_result(result);;
    }else{
        mysql_free_result(result);
    }
    return ret;
}

//put current value in database (int8) // C dont allow overloading
unsigned char put_into_database(char opis[30],int8_t value){
    
    char query[200];
    if (exist_in_database(opis)){
        struct tm * timeinfo;
        time_t rawtime;
        time (&rawtime);
        timeinfo = localtime(&rawtime);
        char buffer[80];
        strftime(buffer,sizeof(buffer),"%Y-%m-%d %H:%M:%S",timeinfo);
        snprintf(query,sizeof(query),"UPDATE `aktualne_wartosci` SET `wartosc`='%d', `data_aktualizacji`='%s' WHERE `nazwa`='%s'",value,buffer,opis);
        //printf(query);
        //printf("\r\n"); 
        if (mysql_query(mysql_con,query)){printf("Blad wykonania zmiany tej wartosci %s\r\n",opis);DATABASEerror++;return 0;}    
    }else{
        snprintf(query,sizeof(query),"INSERT INTO aktualne_wartosci(`nazwa`,`wartosc`) VALUES ('%s','%d')",opis,value);
        if (mysql_query(mysql_con,query)){printf("Blad wykonania dodania tej wartosci %s\r\n",opis);DATABASEerror++;return 0;}    
    }
    MYSQL_RES *result = mysql_store_result(mysql_con);
    mysql_free_result(result);
    
return 1;
}


void Process_datagram(){
    char buff[10];
    unsigned char i;
    unsigned char  offset;
    
    printf("To : ");
    switch (Rx_buf[0]){
        case 0x00:printf("00");break;
        case 0x01:printf("01 (piec)");break;
        case 0x02:printf("02 (lcd)");break;
        default:printf("0x%02X",Rx_buf[0]);break;
    }

    printf("   From : ");
    switch (Rx_buf[1]){
        case 0x00:printf("00");break;
        case 0x01:printf("01 (stove)");break;
        case 0x02:printf("02 (lcd)");break;
        default:printf("0x%02X",Rx_buf[1]);break;
    }

    printf("     Lenght : %u",Rx_buf[2]);
    printf("     Some flags (?): 0x%02X",Rx_buf[3]);
    printf("     Ident. (?): %u",Rx_buf[4]);
    printf("\n");
    
    for (i=0;i<Rx_bufindex;i++)printf("%02X ",Rx_buf[i]);        

    printf("\n");
    if (Rx_buf[4]==0x04){
        DataType4_received = true ;
        //6 - Piec 12-niewiadomo 18-cwu 24 ?? 30-Tempzewn, 36 zaworu 42-poziom paliwa , 48- plomien
        //printf("Temp.Pieca:%u, Temp CWU:%u, Temp. zewn:%d,  Temp.zaworu:%d, Poziom paliwa:%u, Płomień:%u\%, wentylator:%u\%",Rx_buf[6],Rx_buf[6+12],Rx_buf[6+12+12],Rx_buf[6+12+12+6],Rx_buf[6+12+12+12],Rx_buf[48],Rx_buf[60]);
        printf("StoveTemp:%u,ReversTemp:%u, Temp.CWU:%u, TorchTemp:%u, OutTemp:%d, ValveTemp:%d, FuelLvl:%u, Flame:%u\%, Power:%.1fkW, Vent:%u\%",\
                Rx_buf[6],    Rx_buf[12],  Rx_buf[18],  Rx_buf[24], (int8_t) Rx_buf[30],Rx_buf[36],   Rx_buf[42],       Rx_buf[48],    Rx_buf[54]/10.0, Rx_buf[60]);

        put_into_database("Temperatura_pieca",Rx_buf[6]);
        put_into_database("Temperatura powr.",Rx_buf[12]);
        put_into_database("Temperatura_CWU",Rx_buf[18]);
        put_into_database("Temperatura palnika",Rx_buf[24]);   
        put_into_database("Temperatura_zewnetrzna",(int8_t)Rx_buf[30]);
        put_into_database("Temperatura_zaworu",Rx_buf[36]);
        put_into_database("Poziom_paliwa",Rx_buf[42]);
        put_into_database("Plomien",Rx_buf[48]);
        put_into_databasef("Moc [kW]",(float)Rx_buf[54]/10.0);
        put_into_database("Wentylator",Rx_buf[60]);

        stove_data.Temperatura_pieca=Rx_buf[6];
        stove_data.Temperatura_powrotu=Rx_buf[12];
        stove_data.Temperatura_CWU=Rx_buf[18];
        stove_data.Temperatura_palnika=Rx_buf[24];
        stove_data.Temperatura_zewnetrzna=Rx_buf[30];
        stove_data.Temperatura_zaworu=Rx_buf[36];
        stove_data.Poziom_paliwa=Rx_buf[42];
        stove_data.Plomien=Rx_buf[48];
        stove_data.Moc=Rx_buf[54];
        stove_data.Wentylator=Rx_buf[60];

    }else if  (Rx_buf[4]==0x05){
        DataType5_received = true ;
        for (offset=6;offset<Rx_bufindex;offset+=6){
            printf("Status%u,value=%02X\r\n",offset/6,Rx_buf[offset]);
            snprintf(buff,10,"Status%u",offset/6);
        }
        put_into_database("Status_1",Rx_buf[6]);
        put_into_database("Status_2",Rx_buf[27]);

        #define Praca 0x01
        #define Tryb_reczny 0x02
        #define Alarm 0x04
        #define Rozpalanie 0x08
        #define Tajne 0x10  
        #define Testowy 0x20
        //printf("Status1 decoded - Praca:%u,Tryb_reczny:%u,Alarm:%u,Faza_rozpilana:%u,Tryb_testowy:%u\r\n",Rx_buf[6]&0x01,Rx_buf[6]&0x02,Rx_buf[6]&0x04,Rx_buf[6]&0x08,Rx_buf[6]&0x20);

        (Rx_buf[6]&Rozpalanie)?put_into_database("Rozpalanie",1):put_into_database("Rozpalanie",0);
        (Rx_buf[6]&Alarm)?put_into_database("Alarm",1):put_into_database("Alarm",0);
        (Rx_buf[6]&Praca)?put_into_database("Praca",1):put_into_database("Praca",0);


        stove_data.Status_1=Rx_buf[6];
        stove_data.Status_2=Rx_buf[22];
        stove_data.Status_3=Rx_buf[27];
    }
    printf("\n\n");
}
    
    
unsigned short CRC16=0;

#define PRESET_VALUE 0xFFFF
#define POLYNOMIAL  0xa001
//#define POLYNOMIAL  0x8005

unsigned int uiCrc16Cal(unsigned char *pucY, unsigned char ucX){
    unsigned char ucI,ucJ;
    unsigned short int  uiCrcValue = PRESET_VALUE;
    for(ucI = 0; ucI < ucX; ucI++){
        uiCrcValue = uiCrcValue ^ *(pucY + ucI);
        for(ucJ = 0; ucJ < 8; ucJ++){
            if(uiCrcValue & 0x0001){
                uiCrcValue = (uiCrcValue >> 1) ^ POLYNOMIAL;
            }else{
                uiCrcValue = (uiCrcValue >> 1);
            }
        }
    }
    return uiCrcValue;
}

char Open_stove_uart(){
    printf("/dev/ttyUSBpiec\r\n");
    Stove_uart_filestream = open("/dev/ttyUSBpiec", O_RDWR | O_NOCTTY | O_NDELAY);		//Open in non blocking read/write mode
    if (Stove_uart_filestream == -1){printf("Error - Unable to open UART.  Ensure it is not in use by another application\n");return -1;}
    struct termios options;
    tcgetattr(Stove_uart_filestream, &options);
    options.c_cflag = B57600 | CS8 | CLOCAL | CREAD | PARENB;		//<Set baud rate
    options.c_cflag &= ~(PARODD | CSTOPB);
    options.c_iflag = IGNPAR;//PARENB;
    options.c_oflag = 0;
    options.c_oflag &= ~OPOST;
    options.c_lflag = 0;
    tcflush(Stove_uart_filestream, TCIFLUSH);
    tcsetattr(Stove_uart_filestream, TCSANOW, &options);
    return 1;
}



unsigned char Start_char_read=0;
unsigned char czesc_danych=0;
unsigned char Rx_char_read2=0;
char Rx_char_read;
unsigned char bajt=0;
unsigned char ktory_bajt=0;
unsigned char UART_error=0;

void Receive_data_from_stoveUART(){
    if (Stove_uart_filestream != -1){//jesli jest wskaxnik
        while((read(Stove_uart_filestream,&Rx_char_read, 1)>0)&&(!Rx_flag_lineready)){//dopóki możemy odebrać dane    
            if ((Start_char_read)&&(Rx_char_read==0x15)){
                Start_char_read=0;
                Rx_flag_lineready=1;
            }
            
            if (Start_char_read){
                if (ktory_bajt==0){
                    bajt=Rx_char_read<<4;
                    ktory_bajt=1;
                }else if (ktory_bajt==1){
                    bajt|=Rx_char_read;
                    ktory_bajt=2;
                    Rx_char_read2=bajt;
                }
                
                if (ktory_bajt==2){
                    Rx_buf[Rx_bufindex]=Rx_char_read2;
                    Rx_bufindex++;
                    ktory_bajt=0;
                }
                
            }
            
            if (Rx_char_read==0x1A){
                Start_char_read=1;
                Rx_bufindex=0;
            }
        }//while        
    }//if
    
    //checking CRC
    if (Rx_flag_lineready){
        CRC16=uiCrc16Cal(Rx_buf+2,Rx_bufindex-4);
        if ( (((CRC16&0xFF)!=Rx_buf[Rx_bufindex-2]))||(((CRC16>>8)!=Rx_buf[Rx_bufindex-1]))){
            Rx_flag_lineready=0;
            printf("badCRC %d omiting CRCobl=%04X CRCotrzymane=%04X\n",UART_error,CRC16,((unsigned short)Rx_buf[Rx_bufindex-1])<<8|Rx_buf[Rx_bufindex-2]);
            UART_error++;
            if (UART_error==10){ // reopen UART if
                close(Stove_uart_filestream);
                printf("UART Fail - reopening \r\n");
                sleep(2);
                Open_stove_uart();
            }
            if (UART_error>20)abort();
        }else{
            UART_error=0;
        }
    }
    
}


unsigned char Send2Stove(unsigned char * data,unsigned char len){
    len=len+1;
    unsigned short CRC16=0;
    unsigned char package[255];
    package[0]=0x1A;
    unsigned char i=0;
    unsigned char i_package=1;
    
    for (i=0;i<len;i++){
        package[i_package]=(*(data+i))>>4;
        i_package++;
        package[i_package]=(*(data+i))&0x0F;
        i_package++;
    }
    
    CRC16=uiCrc16Cal(data+2,len-2);
    package[i_package]=(CRC16&0xFF)>>4;
    i_package++;
    package[i_package]=(CRC16&0x0F);
    i_package++;


    package[i_package]=CRC16>>12;
    i_package++;
    package[i_package]=(CRC16>>8)&0x0F;
    i_package++;


    package[i_package]=0x15;
    i_package++;

    printf("Sending ->");
    for (i=0;i<i_package;i++){
        printf("%02X ",package[i]);
    }
    write(Stove_uart_filestream,package,i_package);
    printf("\r\n");
   
}