#include "types.h"
#include "user.h"
#include "date.h"

int 
main ( int argc , char * argv []){
	struct rtcdate r;
	if(date(&r)){ //si date devuelve error 
		printf(2, "date failed\n");
		exit();
	}
	printf(1,"La hora actual es: %d:%d:%d\n", r.hour+1,r.minute,r.second); //hh:mm:ss
	exit();
}
