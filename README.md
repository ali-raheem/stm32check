# stm32check
Characterize STM32F1/4 clones

STM32check is an arduino sketch that runs on STM32(clones) and attempts to identify features as well as manufactuer.

Checks FLASH size is as reported, RAM size is as reported, which timer's are present and if a GD32 style RNG is present.

Simply upload to the STM32 via STLink or STM32duino and monnitor over serial port. It'll reboot as needed.

This is about as small as I could make it to fit in my AliExpress specials that misreport flash size, but you can trim down the strings.
