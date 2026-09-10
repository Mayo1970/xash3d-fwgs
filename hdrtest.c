#include <stdio.h>
#include "ps3_sound_preload.h"
int main(void){
    int n=(int)(sizeof(ps3_sound_preload)/sizeof(ps3_sound_preload[0]));
    for(int i=0;i<n;i++) printf("%s %d first=%s\n", ps3_sound_preload[i].gamedir, ps3_sound_preload[i].count, ps3_sound_preload[i].names[0]);
    return 0;
}
