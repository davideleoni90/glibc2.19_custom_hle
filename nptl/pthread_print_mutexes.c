/* Get the default attributes used by pthread_create in the process.
   Copyright (C) 2013-2014 Free Software Foundation, Inc.
   This file is part of the GNU C Library.

   The GNU C Library is free software; you can redistribute it and/or
   modify it under the terms of the GNU Lesser General Public
   License as published by the Free Software Foundation; either
   version 2.1 of the License, or (at your option) any later version.

   The GNU C Library is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Lesser General Public License for more details.

   You should have received a copy of the GNU Lesser General Public
   License along with the GNU C Library; if not, see
   <http://www.gnu.org/licenses/>.  */

#include <errno.h>
#include <stdlib.h>
#include <pthreadP.h>
#include <stdio.h>

#define TOP_CONTENDED 20


// (dleoni) Array with top contended mutexes
struct DataItem *top_contended[TOP_CONTENDED];

// (dleoni) Fill the array with the first TOP_CONTENDED elements; keep it sorted
void first_elements() {
   unsigned int free = TOP_CONTENDED;
   unsigned int i = 0;
   while(free > 0) {
      if(!hashArray[i]) {
         i += 1;
         continue;
      }
      float current = hashArray[i]->mutex_contention/hashArray[i]->acquisitions;
      // First case: the current element is more contended than the current most contended
      if((!top_contended[0]) || (current >= (top_contended[0]->mutex_contention/top_contended[0]->acquisitions))) {
         for(int j = TOP_CONTENDED - free; j > 0 ; j--) {
            top_contended[j] = top_contended[j-1];
         }  
         top_contended[0] = hashArray[i];
         free -= 1;
         i += 1;
         continue;
      }
      // Second case: the current element is less contenede than the least contended
      float last = top_contended[TOP_CONTENDED - free - 1]->mutex_contention/top_contended[TOP_CONTENDED - free - 1]->acquisitions;
      if(current <= last) {
         top_contended[TOP_CONTENDED - free] = hashArray[i];
         free -= 1;
         i += 1;
         continue;
      }
      // Intermediate case: the contention of the current element is in between the above cases
      for(unsigned int j = 0; j < TOP_CONTENDED - free; j++) {
         float element = top_contended[j]->mutex_contention/top_contended[j]->acquisitions;
         if(current >= element) {
            for(unsigned int k = TOP_CONTENDED - free; k > j; k--) {
               top_contended[k] = top_contended[k-1];
            }
            top_contended[j] = hashArray[i];
            free -= 1;
            i += 1;
            break;
         }
      }
   }
}

// (dleoni) Insert the mutex (corresponding to the given index) in the list of top-contended mutexes mantaining the order
void insert_ordered(unsigned int index) {
   float current, last;
   current = hashArray[index]->mutex_contention/hashArray[index]->acquisitions;
   last = top_contended[TOP_CONTENDED - 1]->mutex_contention/top_contended[TOP_CONTENDED - 1]->acquisitions;
   if(current < last) {
      return;
   }        
   for(unsigned int j = 0; j < TOP_CONTENDED; j++) {
      float element = top_contended[j]->mutex_contention/top_contended[j]->acquisitions;
      if(current >= element) {
         for(unsigned int k = TOP_CONTENDED - 1; k > j; k--) {
            top_contended[k] = top_contended[k-1];
         }
         top_contended[j] = hashArray[index];
         break;
      }
   } 
}

// (dleoni) Show statistics about mutexes
void pthread_print_mutexes(void) {

   unsigned int index;
   float avg_contention;

   // stop measuring contention of mutexes
   measure_mutexes_contention = false;
  
   // order the first TOP_CONTENDED mutexes by decreasing average contention
   first_elements();
   
   // add the remaining mutexes
   for(index = TOP_CONTENDED; index < MAX_NUMBER_MUTEXES; index++) {
      if(hashArray[index]) {
          insert_ordered(index);
      }
   }

   // Print the TOP_CONTENDED mutexes
   printf("TOP-CONTENDED MUTEXES\n");
   printf("Address\tAvg_acquisition_time(nanoseconds)\tTot_acquisition_time\tAcquisitions\n");
   for(int i = 0; i < TOP_CONTENDED; i++) {
      struct DataItem *current = top_contended[i];
      avg_contention = current->mutex_contention/current->acquisitions;
      printf("%p\t%f\t%ld\t%d\n", current->mutex_address, avg_contention, current->mutex_contention, current->acquisitions);
   }

/*
   for(index = 0; index < MAX_NUMBER_MUTEXES; index++) {
      if(hashArray[index]) {
         int i = 0;
         struct DataItem *next = hashArray[index];
         while (next != NULL) {
            avg_contention = next->mutex_contention/next->acquisitions;
            printf("MUTEX:%p; AVG_CONTENTION:%f; INDEX:%d; TOT_CONTENTION:%ld; ACQUISITIONS:%d\n", next->mutex_address, avg_contention, index, next->mutex_contention, next->acquisitions);
            next = next->next;
            i += 1;
         }
         if (i > 1) {
            printf("Bucket:%d\n", i);
         }
         fflush(stdout);
      }
   }*/
   printf("TOTAL COLLISIONS:%d\n", number_of_collisions);
   fflush(stdout);
}




