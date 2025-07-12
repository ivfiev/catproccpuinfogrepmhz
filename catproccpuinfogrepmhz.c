#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <ctype.h>
#include <signal.h>
#include <math.h>
#include <sys/time.h>

#define SAMPLES_PER_SEC 10
#define SAMPLES_RING 50

#define OFFSET_POWER_UNIT 0xc0010299
#define OFFSET_CORE_ENERGY 0xc001029a
#define OFFSET_PACKAGE_ENERGY 0xc001029b

int get_cpus(void) {
	char buf[1024];
	char *line;
	FILE *fp;

	fp = popen("lscpu", "r"); 
	if (fp == NULL) {
		printf("failed to run lscpu\n");
		exit(1);
	}

	for (;;) {
		char *s = fgets(buf, sizeof(buf), fp);
		if (s == NULL) {
			printf("failed to read lscpu output\n");
			exit(1);
		}

		line = strcasestr(s, "CPU(s)");
		if (line != NULL) {
			break;
		}
	}

	while (*line && !isdigit(*line)) {
		line++;
	}

	if (!*line) {
		printf("failed to parse the number of CPUs\n");
		exit(1);
	}

	pclose(fp);
	return atoi(line);
}

char *read_cpuinfo(void) {
	const int size = 64 * 1024;
	char *buf = calloc(size, sizeof(char));
	int fp, bytes, len;

	len = 0;
	fp = open("/proc/cpuinfo", O_RDONLY);
	while ((bytes = read(fp, buf + len, size - 1 - len)) > 0) {
		len += bytes;
	}

	if (bytes < 0) {
		printf("failed to read cpuinfo\n");
		exit(1);
	}

	close(fp);
	return buf;
}

void index_cpuinfo(int indexes[], int cpus) {
	char *cpuinfo = read_cpuinfo();
	char *ix = cpuinfo;
	int cpu = 0;

	while (cpu < cpus) {
		ix = strstr(ix, "cpu MHz");
		if (ix == NULL) {
			printf("failed to index cpuinfo\n");
			exit(1);
		}
		indexes[cpu++] = ix - cpuinfo;
		ix++;
	}

	free(cpuinfo);
}

void read_clocks(int clocks[], int indexes[], int cpus) {
	char *cpuinfo = read_cpuinfo();

	for (int i = 0; i < cpus; i++) {
		char *ix = cpuinfo + indexes[i] - cpus;
		ix = strstr(ix, "MHz");

		if (ix == NULL) {
			printf("failed to parse indexed cpuinfo\n");
			exit(1);
		}
		while (!isdigit(*ix)) {
			ix++;
		}
		clocks[i] = atoi(ix);
	}

	free(cpuinfo);
}

void cleanup(int) {
	(void)system("clear");
	exit(0);
}

void handle_sigint(void) {
	struct sigaction a;
	memset(&a, 0, sizeof(a));
	a.sa_handler = cleanup;
	if (sigaction(SIGINT, &a, NULL) < 0) {
		printf("signal handler failed\n");
	}
}

void calc_ring_stats(int ring[][SAMPLES_RING], int maxes[], float avgs[], int cpus) {
	for (int i = 0; i < cpus; i++) {
		int max = 0;
		float avg = 0;
		for (int j = 0; j < SAMPLES_RING; j++) {
			max = ring[i][j] > max ? ring[i][j] : max;
			avg += ring[i][j];
		}
		avg /= SAMPLES_RING;
		avgs[i] = avg;
		maxes[i] = max;
	}
}

void calc_loads(int min_freq, float avgs[], int loads[], int cpus) {
	const int stages = 5;
	float max = -10e9;
	for (int i = 0; i < cpus; i++) {
		max = avgs[i] > max ? avgs[i] : max;
	}
	float range = ceil(1 + (max - min_freq) / stages);
	for (int i = 0; i < cpus; i++) {
		loads[i] = 1 + (int)((avgs[i] - min_freq) / range);
	}
}

int open_msr(int cpu) {
	char str[32];
	snprintf(str, sizeof(str), "/dev/cpu/%d/msr", cpu);
	int fd = open(str, O_RDONLY);
	return fd;
}

uint64_t read_msr(int fd, __off64_t offset) {
	uint8_t buf[8];
	int read = pread(fd, buf, sizeof(buf), offset);
	if (read != 8) {
		perror("could not read msr");
	}
	return *(uint64_t *)buf;
}

int init_power_draw(int *fds, float *energy_unit, int cpus) {
	for (int i = 0; i < cpus; i++) {
		int fd = open_msr(i);
		if (fd < 0) {
			return -1;
		}
		fds[i] = fd;
	}
	uint64_t unit = read_msr(fds[0], OFFSET_POWER_UNIT);
	*energy_unit = 1.0 / (1 << ((unit >> 8) & 0x1F));
	return 0;
}

void read_power_draw(int *fds, uint64_t *pkg0, uint64_t *pkg1, uint64_t *cpu0, uint64_t *cpu1, int cpus) {
	*pkg1 = *pkg0;
	*pkg0 = read_msr(fds[0], OFFSET_PACKAGE_ENERGY);
	for (int i = 0; i < cpus; i++) {
		cpu1[i] = cpu0[i];
		cpu0[i] = read_msr(fds[i], OFFSET_CORE_ENERGY);
	}
}

float watts(float energy_unit, uint64_t elapsed_usec, uint64_t unit0, uint64_t unit1) {
	if (unit0 == 0 || unit1 == 0 || elapsed_usec == 0) {
		return 0;
	}
	uint64_t delta = unit0 - unit1;
	return delta * energy_unit / (elapsed_usec / 1000000.0);
}

uint64_t now_usec() {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    uint64_t usec = (long long)tv.tv_sec * 1000000 + tv.tv_usec;
    return usec;
}

int main(void) {
	handle_sigint();
	int cpus = get_cpus(), samples = 0;
	int indexes[cpus], clocks[cpus], maxes[cpus], maxes_ring[cpus], ring[cpus][SAMPLES_RING], loads[cpus];
	int fds[cpus];
	float avgs[cpus], avgs_ring[cpus];
	uint64_t pkg0 = 0, pkg1 = 0;
	uint64_t cpu0[cpus], cpu1[cpus];
	float energy_unit;
	int min_avg_freq = 0xFFFF;

	memset(avgs, 0, cpus * sizeof(float));
	memset(maxes, 0, cpus * sizeof(int));
	memset(ring, 0, cpus * SAMPLES_RING * sizeof(int));
	memset(loads, 0, cpus * sizeof(int));
	memset(cpu0, 0, cpus * sizeof(uint64_t));
	memset(cpu1, 0, cpus * sizeof(uint64_t));
	index_cpuinfo(indexes, cpus);
	int power_draw = init_power_draw(fds, &energy_unit, cpus);

	for (;;) {
		uint64_t start_usec = now_usec();
		for (int i = 0; i < SAMPLES_PER_SEC; i++) {
			read_clocks(clocks, indexes, cpus);
			
			for (int cpu = 0; cpu < cpus; cpu++) {
				maxes[cpu] = maxes[cpu] < clocks[cpu] ? clocks[cpu] : maxes[cpu];
				avgs[cpu] = (avgs[cpu] * samples + clocks[cpu]) / (samples + 1);
				ring[cpu][samples % SAMPLES_RING] = clocks[cpu];
			}

			samples++;
			usleep(1000000 / SAMPLES_PER_SEC);
		}
		uint64_t end_usec = now_usec();
		uint64_t elapsed_usec = end_usec - start_usec;

		read_power_draw(fds, &pkg0, &pkg1, cpu0, cpu1, cpus);

		printf("\e[1;1H\e[2J");
		printf("core#\tnow\tmax(%d)\tavg(%d)\tmax(*)\tavg(*)\ttpd(%.1fw)\n", SAMPLES_RING, SAMPLES_RING, watts(energy_unit, elapsed_usec, pkg0, pkg1));

		calc_ring_stats(ring, maxes_ring, avgs_ring, cpus);
		
		if (samples >= SAMPLES_RING) {
			for (int i = 0; i < cpus; i++) {
				min_avg_freq = avgs_ring[i] < min_avg_freq ? avgs_ring[i] : min_avg_freq;
			}

			calc_loads(min_avg_freq, avgs_ring, loads, cpus);
		}

		for (int i = 0; i < cpus; i++) {
			printf("%d\t%d\t%d\t%d\t%d\t%d\t%.1fw\t\t", i, clocks[i], maxes_ring[i], (int)avgs_ring[i], maxes[i], (int)avgs[i], watts(energy_unit, elapsed_usec, cpu0[i], cpu1[i]));

			for (int j = 0; j < loads[i]; j++) {
				putchar('*');
			}

			putchar('\n');
		}
	}

	return 0;
}
