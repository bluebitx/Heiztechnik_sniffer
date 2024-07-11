Programs alows to read data from Heiztechnik stove and put data info mariaDB

I spend a lot of time to read data from BUS - that is nowhere described. 

Brager Bus (protocol used by Heiztechnik devices) is simple RS485 comunication betwen stove and remote devices (like displays and remote controler).

I use it to repair some bugs in their controlers and improve work of this device.

Abilitty of this code :
- Reading temperatures from stove,
- Ability to start,stop ect. stove via remote command,
- Putting data to mariaDB,
- Turn off stove on next run 
  After reaching proper temperature device going to sleep mode, after that water temp. decresing. till some point of temp. 
  i turn off stove before next start,
- Auto turn off (in summer mode) - after water in conteiner is ready,
- Auto turn on at defined hour,



