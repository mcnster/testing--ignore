#define INPUT_PORTS 8
#define OUTPUT_PORTS 8

typedef struct _InfoBlock {
   unsigned long long int frame;
   unsigned int transport_rolling;
   unsigned int priority;
   unsigned int running;
   unsigned int inputs;
   unsigned int outputs;
   unsigned int buffer_frames;
   unsigned int sample_rate;
} InfoBlock;

