#include <iostream>
#include <fstream>
#include <thread>
#include <random>

using std::cout, std::endl, std::this_thread::sleep_for;
using namespace std::chrono_literals;
std::ofstream logfile;
									            //		      YOU CAN SET THESE CONSTS WHILE DEBUGING
const int TIME_TO_SLEEP = 0;					//  <======== Animation delay (ms)
const bool PRINT_STEPS = true;					//  <======== Enable / disable animation
const int STEPS_LIMIT =  256;					//  <======== Steps limit for each run
const int N_TESTS = 10;						    //  <======== Number of each test runs (>=100 for statistics)

//	=========================================  YOUR CODE HERE  ==========================================
//	|	|	|	|	|	|	|	|	|	|	|	|	|	|	|	|	|	|	|	|	|	|	|	|	|	|
//	v	v	v	v	v	v	v	v	v	v	v	v	v	v	v	v	v	v	v	v	v	v	v	v	v	v

int direction = 0;                              //  <======== Your global values.

void Reset()                                    //  <======== You can set or reset you globals
{                                               //            before each test.
	direction = 0;
}

// void MyOwnFunc(){}                           //  <======== You can add you functions here.

char Move(char map[][8], int ant_x, int ant_y, int home_x, int home_y, int sample_x, int sample_y) // <======= YEP, THIS IS IT
{
	struct State
	{
		unsigned long long rocks;
		unsigned char ant;
		unsigned char sample;
	};

	struct Node
	{
		unsigned long long rocks;
		unsigned char ant;
		unsigned char sample;
		int parent;
		short g;
		short h;
		short f;
		unsigned char move;
	};

	const int HOME = home_x * 8 + home_y;
	const int START_ANT = ant_x * 8 + ant_y;
	const int START_SAMPLE = sample_x * 8 + sample_y;
	unsigned long long startRocks = 0;

	for (int i = 0; i < 8; i++)
		for (int j = 0; j < 8; j++)
			if (map[i][j] == '#')
				startRocks |= (1ULL << (i * 8 + j));

	static char plan[300];
	static int planLen = 0;
	static int planPos = 0;
	static State expected = { 0, 255, 255 };
	static int avoidSample = -1;
	State current = { startRocks, (unsigned char)START_ANT, (unsigned char)START_SAMPLE };

	auto sameState = [](const State& a, const State& b) -> bool
	{
		return a.rocks == b.rocks && a.ant == b.ant && a.sample == b.sample;
	};

	auto inside = [](int x, int y) -> bool
	{
		return x >= 0 && x < 8 && y >= 0 && y < 8;
	};

	auto posManhattan = [](int a, int b) -> int
	{
		int ax = a / 8, ay = a % 8;
		int bx = b / 8, by = b % 8;
		int dx = ax > bx ? ax - bx : bx - ax;
		int dy = ay > by ? ay - by : by - ay;
		return dx + dy;
	};

	auto makeMove = [&](const State& from, int dir, State& to, bool& finish) -> bool
	{
		static const int dx[4] = { -1, 1, 0, 0 };
		static const int dy[4] = { 0, 0, -1, 1 };
		int ax = from.ant / 8, ay = from.ant % 8;
		int tx = ax + dx[dir], ty = ay + dy[dir];
		finish = false;
		if (!inside(tx, ty)) return false;

		int target = tx * 8 + ty;
		int twoX = ax + 2 * dx[dir], twoY = ay + 2 * dy[dir];
		int pullX = ax - dx[dir], pullY = ay - dy[dir];
		bool twoInside = inside(twoX, twoY);
		bool pullInside = inside(pullX, pullY);
		int two = twoX * 8 + twoY;
		int pull = pullX * 8 + pullY;
		unsigned long long targetMask = 1ULL << target;

		to = from;

		if (target == from.sample)
		{
			if (!twoInside) return false;
			if (from.rocks & (1ULL << two)) return false;
			if (two == HOME)
			{
				finish = true;
				return true;
			}
			to.ant = (unsigned char)target;
			to.sample = (unsigned char)two;
			return true;
		}

		if (from.rocks & targetMask)
		{
			if (!twoInside) return false;
			if (two == HOME || two == from.sample) return false;
			if (from.rocks & (1ULL << two)) return false;
			to.rocks &= ~targetMask;
			to.rocks |= (1ULL << two);
			to.ant = (unsigned char)target;
			return true;
		}

		to.ant = (unsigned char)target;
		if (!pullInside) return true;

		unsigned long long pullMask = 1ULL << pull;
		if (pull == from.sample)
		{
			if (from.ant == HOME)
			{
				finish = true;
				return true;
			}
			to.sample = from.ant;
			return true;
		}
		if (from.rocks & pullMask)
		{
			if (from.ant == HOME) return true;
			to.rocks &= ~pullMask;
			to.rocks |= (1ULL << from.ant);
		}
		return true;
	};

	if (!(planPos < planLen && sameState(current, expected)))
	{
		planLen = 0;
		planPos = 0;

		const int MAX_NODES = 1500000;
		const int HASH_SIZE = 4194304;
		static Node nodes[MAX_NODES];
		static int heap[MAX_NODES];
		static unsigned long long hashRocks[HASH_SIZE];
		static unsigned short hashPos[HASH_SIZE];
		static int hashValue[HASH_SIZE];
		static unsigned int hashStamp[HASH_SIZE];
		static unsigned int stamp = 0;
		stamp++;
		if (stamp == 0)
		{
			for (int i = 0; i < HASH_SIZE; i++) hashStamp[i] = 0;
			stamp = 1;
		}

		const int startDistance = posManhattan(current.sample, HOME);
		auto heuristic = [&](const State& st) -> int
		{
			int bestAnt = 20;
			int sx = st.sample / 8, sy = st.sample % 8;
			static const int hx[4] = { -1, 1, 0, 0 };
			static const int hy[4] = { 0, 0, -1, 1 };
			for (int i = 0; i < 4; i++)
			{
				int nx = sx + hx[i], ny = sy + hy[i];
				if (inside(nx, ny))
				{
					int p = nx * 8 + ny;
					int d = posManhattan(st.ant, p);
					if (d < bestAnt) bestAnt = d;
				}
			}
			return 100 * posManhattan(st.sample, HOME) + 4 * bestAnt;
		};

		auto heapLess = [&](int a, int b) -> bool
		{
			if (nodes[a].f != nodes[b].f) return nodes[a].f < nodes[b].f;
			return nodes[a].h < nodes[b].h;
		};

		int heapSize = 0;
		auto heapPush = [&](int value)
		{
			int p = heapSize++;
			heap[p] = value;
			while (p > 0)
			{
				int q = (p - 1) / 2;
				if (heapLess(heap[q], heap[p])) break;
				int t = heap[p]; heap[p] = heap[q]; heap[q] = t;
				p = q;
			}
		};

		auto heapPop = [&]() -> int
		{
			int result = heap[0];
			heap[0] = heap[--heapSize];
			int p = 0;
			while (true)
			{
				int l = 2 * p + 1, r = l + 1, b = p;
				if (l < heapSize && heapLess(heap[l], heap[b])) b = l;
				if (r < heapSize && heapLess(heap[r], heap[b])) b = r;
				if (b == p) break;
				int t = heap[p]; heap[p] = heap[b]; heap[b] = t;
				p = b;
			}
			return result;
		};

		auto hashFindOrAdd = [&](const State& st, int newIndex) -> int
		{
			unsigned short packed = (unsigned short)(((int)st.ant << 6) | (int)st.sample);
			unsigned long long h = st.rocks;
			h ^= (unsigned long long)packed * 11995408973635179863ULL;
			h ^= h >> 32;
			int slot = (int)(h & (HASH_SIZE - 1));
			while (hashStamp[slot] == stamp)
			{
				if (hashRocks[slot] == st.rocks && hashPos[slot] == packed)
					return hashValue[slot];
				slot = (slot + 1) & (HASH_SIZE - 1);
			}
			hashStamp[slot] = stamp;
			hashRocks[slot] = st.rocks;
			hashPos[slot] = packed;
			hashValue[slot] = newIndex;
			return -1;
		};

		nodes[0].rocks = current.rocks;
		nodes[0].ant = current.ant;
		nodes[0].sample = current.sample;
		nodes[0].parent = -1;
		nodes[0].g = 0;
		nodes[0].h = (short)heuristic(current);
		nodes[0].f = nodes[0].h;
		nodes[0].move = 0;
		int nodeCount = 1;
		hashFindOrAdd(current, 0);
		heapPush(0);

		static const char moveChar[4] = { 'u', 'd', 'l', 'r' };
		int goalNode = -1;
		char goalMove = 0;
		int sideNode = -1;
		char sideMove = 0;
		int sideScore = 1000000;
		int awayNode = -1;
		char awayMove = 0;
		int awayScore = 1000000;

		while (heapSize > 0 && nodeCount < MAX_NODES)
		{
			int index = heapPop();
			if (nodes[index].g >= 255) continue;

			State base = { nodes[index].rocks, nodes[index].ant, nodes[index].sample };
			for (int d = 0; d < 4; d++)
			{
				State next;
				bool finish = false;
				if (!makeMove(base, d, next, finish)) continue;
				if (finish)
				{
					goalNode = index;
					goalMove = moveChar[d];
					heapSize = 0;
					break;
				}
				int nextDistance = posManhattan(next.sample, HOME);
				if (next.sample != base.sample && nextDistance < startDistance)
				{
					goalNode = index;
					goalMove = moveChar[d];
					heapSize = 0;
					break;
				}
				if (next.sample != base.sample && (int)next.sample != avoidSample)
				{
					int score = nodes[index].g * 10 + posManhattan(next.ant, HOME);
					if (nextDistance == startDistance && score < sideScore)
					{
						sideScore = score;
						sideNode = index;
						sideMove = moveChar[d];
					}
					if (nextDistance == startDistance + 1 && score < awayScore)
					{
						awayScore = score;
						awayNode = index;
						awayMove = moveChar[d];
					}
				}
				if (hashFindOrAdd(next, nodeCount) != -1) continue;
				nodes[nodeCount].rocks = next.rocks;
				nodes[nodeCount].ant = next.ant;
				nodes[nodeCount].sample = next.sample;
				nodes[nodeCount].parent = index;
				nodes[nodeCount].g = nodes[index].g + 1;
				nodes[nodeCount].h = (short)heuristic(next);
				nodes[nodeCount].f = nodes[nodeCount].g + nodes[nodeCount].h;
				nodes[nodeCount].move = moveChar[d];
				heapPush(nodeCount);
				nodeCount++;
			}
		}

		if (goalNode == -1 && sideNode != -1)
		{
			goalNode = sideNode;
			goalMove = sideMove;
		}
		if (goalNode == -1 && awayNode != -1)
		{
			goalNode = awayNode;
			goalMove = awayMove;
		}

		if (goalNode != -1)
		{
			char reverse[300];
			int count = 0;
			reverse[count++] = goalMove;
			for (int p = goalNode; p != -1 && nodes[p].parent != -1; p = nodes[p].parent)
				reverse[count++] = (char)nodes[p].move;
			for (int i = 0; i < count; i++)
				plan[i] = reverse[count - 1 - i];
			planLen = count;
			expected = current;
			avoidSample = current.sample;
		}
	}

	if (planPos < planLen)
	{
		char answer = plan[planPos++];
		State next;
		bool finish = false;
		int d = answer == 'u' ? 0 : answer == 'd' ? 1 : answer == 'l' ? 2 : 3;
		if (makeMove(current, d, next, finish))
			expected = next;
		logfile << "go " << (char)('A' + answer - 'a') << ' ';
		return answer;
	}

	int best = 0;
	int bestScore = 1000000;
	static const char fallbackMove[4] = { 'u', 'd', 'l', 'r' };
	for (int d = 0; d < 4; d++)
	{
		State next;
		bool finish = false;
		if (!makeMove(current, d, next, finish)) continue;
		if (finish) return fallbackMove[d];
		int score = posManhattan(next.sample, HOME) * 10 + posManhattan(next.ant, next.sample);
		if (score < bestScore)
		{
			bestScore = score;
			best = d;
		}
	}
	return fallbackMove[best];
}

//	^	^	^	^	^	^	^	^	^	^	^	^	^	^	^	^	^	^	^	^	^	^	^	^	^	^
//	|	|	|	|	|	|	|	|	|	|	|	|	|	|	|	|	|	|	|	|	|	|	|	|	|	|
//	=========================================  YOUR CODE HERE  ==========================================



//	========================================= ! DO NOT TOUCH ! ==========================================
//	|	|	|	|	|	|	|	|	|	|	|	|	|	|	|	|	|	|	|	|	|	|	|	|	|	|
//	v	v	v	v	v	v	v	v	v	v	v	v	v	v	v	v	v	v	v	v	v	v	v	v	v	v

void ClearLab(char lab[][8])
{
	for (int i = 0; i < 8; i++)
		for (int j = 0; j < 8; j++)
			lab[i][j] = '.';
}
void PrintLab(char lab[][8])
{
	for (int i = 0; i < 8; i++) {
		for (int j = 0; j < 8; j++)
			printf("%c ", lab[i][j]); // cout << lab[i][j] << ' ';
		printf("\n");// cout << endl;
	}
}
bool MakeLab(char lab[][8], int ax, int ay, int Ox, int Oy, int gx, int gy, int rn, int rx[], int ry[])
{
	ClearLab(lab);
	if ((gx == ax && gy == ay) || (gx == Ox && gy == Oy)) { return false; }

	for (int i=0;i<rn;i++)
		lab[rx[i]][ry[i]] = '#';
	if (ax == Ox && ay == Oy)
		lab[ax][ay] = '@';
	else
	{
		lab[ax][ay] = 'a';
		lab[Ox][Oy] = 'O';
	}
	lab[gx][gy] = '%';
	return true;
}
void CopyLab(char lab[][8], char copy[][8], int* ax, int* ay, int* Ox, int* Oy, int* gx, int* gy)
{
	for (int i = 0; i < 8; i++)
		for (int j = 0; j < 8; j++)
		{
			copy[i][j] = lab[i][j];
			switch (copy[i][j])
			{
			case 'a': *ax = i; *ay = j; break;
			case '%': *gx = i; *gy = j;	break;
			case 'O': *Ox = i; *Oy = j;	break;
			case '@': *ax = i; *ay = j; *Ox = i; *Oy = j; break;
			default: break;
			}
		}
}

bool MakeLab(char lab[][8], int ax, int ay, int Ox, int Oy, int gx, int gy, int rn)
{
	int rx[64] = { 0 };
	int ry[64] = { 0 };

	if (!MakeLab(lab, ax, ay, Ox, Oy, gx, gy, 0, rx, ry))
    {
        logfile<<"GEN-ERR";
        return false;
    }


	int count = rn;
	int stop = 16;
	while (count > 0) {
		for (int i = 0; i < 8; i++)
			for (int j = 0; j < 8; j++)
			{
				if (lab[i][j] == '.' && rand() % 64 < rn && (abs(i - Ox) + abs(j - Oy) > 2))
				{
					lab[i][j] = '#'; count--;
					if (count == 0) return true;
				}
			}
		stop--;
		if (stop == 0)return false;
	}
	return true;
}

int GoTest(char lab[][8], bool doprint)
{
    logfile<<"#\tNew test... ";
	Reset();
	for (int s = 1; s < STEPS_LIMIT+1; s++)
	{
		if (doprint)
		{
			sleep_for(std::chrono::milliseconds(TIME_TO_SLEEP));
			//system("CLS");
			printf("Step: %d / %d                       \n",s, STEPS_LIMIT);
			PrintLab(lab);
			for (int l = 0; l < 9; l++)
			{
				printf("\r\033[A");
			}
		}
		char copy[8][8];
		int ax, ay, Ox, Oy, gx, gy;
		CopyLab(lab, copy, &ax, &ay, &Ox, &Oy, &gx, &gy);
		char c = Move(copy, ax, ay, Ox, Oy, gx, gy);
		if (c=='x') {logfile<<" terminated at step "<<s<<"!\n"; return -1;} //breaking this ant run
		if (c=='q') {logfile<<" terminated!\n"; return -2;} //breaking this ant run

		int tox = ax, toy = ay, totox = ax, totoy = ay, pullx = ax, pully = ay;
		bool toB = true, totoB = true, pullB = true;
		switch (c)
		{
		case 'u': tox--; totox -= 2; pullx++; toB = (tox >= 0); totoB = (totox >= 0); pullB = (pullx < 8); break;
		case 'd': tox++; totox += 2; pullx--; toB = (tox < 8); totoB = (totox < 8); pullB = (pullx >= 0); break;
		case 'l': toy--; totoy -= 2; pully++; toB = (toy >= 0); totoB = (totoy >= 0); pullB = (pully < 8); break;
		case 'r': toy++; totoy += 2; pully--; toB = (toy < 8); totoB = (totoy < 8); pullB = (pully >= 0); break;
		default: //FAIL
			break;
		}
		if (!toB) continue; //wall
		if (lab[tox][toy] == '.' || lab[tox][toy] == 'O') //simple move or pull
		{
			lab[tox][toy] = (lab[tox][toy] == '.') ? 'a' : '@';
			if (!pullB || lab[pullx][pully] == '.' || lab[pullx][pully] == 'O') { lab[ax][ay] = (lab[ax][ay] == 'a') ? '.' : 'O'; continue; }//simple move
			if (lab[pullx][pully] == '%' && lab[ax][ay] == '@') { logfile<<" done in "<<s<<" steps!\n"; return s; }//DONE with pull
			if (lab[pullx][pully] == '#' && lab[ax][ay] == '@') { lab[ax][ay] = 'O';  continue; } //cant pull # to O
			lab[ax][ay] = lab[pullx][pully]; lab[pullx][pully] = '.'; continue;//pull # or %

		}
		if (lab[tox][toy] == '%')
		{
			if (!totoB || lab[totox][totoy] == '#') continue; //wall or blocked
			if (lab[totox][totoy] == '.') { lab[tox][toy] = 'a'; lab[ax][ay] = (lab[ax][ay] == 'a') ? '.' : 'O'; lab[totox][totoy] = '%'; continue; } //push % to .
			if (lab[totox][totoy] == 'O') { logfile<<" done in "<<s<<" steps!\n"; return s; } //DONE with push
		}
		if (lab[tox][toy] == '#')
		{
			if (!totoB || lab[totox][totoy] != '.') continue; //wall or blocked
			lab[tox][toy] = 'a'; lab[ax][ay] = (lab[ax][ay] == 'a') ? '.' : 'O';; lab[totox][totoy] = '#'; continue;  //push # to .
		}
	}
	logfile<<" fail!\n";
	return -1;
}

void StopAll()
{
    cout << "Terminated!" << endl;
	logfile << "#####\tTerminated!\n";
    logfile.close();
    exit(0);
}

int main()
{
    printf("Starting new Antonina runs!\nSTEPS_LIMIT=%d N_TESTS=%d\n",STEPS_LIMIT,N_TESTS);
    logfile.open ("antlog.txt");
    logfile << "#####\tStarting new Antonina runs!\n#####\tSTEPS_LIMIT="<<STEPS_LIMIT<<"\tN_TESTS="<<N_TESTS<<"\n";
	srand(time(NULL));
	char lab[8][8];
	int rx[64];
	int ry[64];
	int nr = 0;
	int wins;
	int sum;
	int score,wr,as;
	int totalscore = 0;

	//Test 00 	one line not at walls
	logfile << "#####\tStarting Test 00...\n";
	wins = 0; sum = 0;
	for (int i = 0; i < N_TESTS; i++)
	{
		int ax = 1 + rand() % 6, ay = 1 + rand() % 6;
		int gx = ax, gy = ay;
		if (rand() % 2 == 0) while (gx == ax) gx = 1 + rand() % 6;
		else while (gy == ay) gy = 1 + rand() % 6;
		MakeLab(lab, ax, ay, ax, ay, gx, gy, 0);
		//test
		printf("Test 00: %d/%d             \n",i,N_TESTS);
		int res = GoTest(lab, PRINT_STEPS);
		if (res > 0) { wins++; sum += res; }
		else if(res==-2) StopAll();
		printf("\r\033[A");
	}
	score = 100*(wins * STEPS_LIMIT - sum)/ STEPS_LIMIT/ N_TESTS;
	totalscore += score;
	wr = 100 * wins / N_TESTS; as = wins > 0 ? sum / wins : 0;
	printf("Test 00: winrate=%d%% av.steps=%d score=%d\n", wr, as, score);
	logfile << "#####\tTest 00: winrate="<<wr<<"% av.steps=" << as<<" score="<<score<<"\n";

	//Test 01 	no line not at walls
	logfile << "#####\tStarting Test 01...\n";
	wins = 0; sum = 0;
	for (int i = 0; i < N_TESTS; i++)
	{
		int ax = 1 + rand() % 6, ay = 1 + rand() % 6;
		int gx = ax, gy = ay;
		while (gx == ax) gx = 1 + rand() % 6;
		while (gy == ay) gy = 1 + rand() % 6;
		MakeLab(lab, ax, ay, ax, ay, gx, gy, 0);
		//test
		printf("Test 01: %d/%d             \n", i, N_TESTS);
		int res = GoTest(lab, PRINT_STEPS);
		if (res > 0) { wins++; sum += res; }
		else if(res==-2) StopAll();
		printf("\r\033[A");
	}
	score = 100 * (wins * STEPS_LIMIT - sum) / STEPS_LIMIT / N_TESTS;
	totalscore += score;
	wr = 100 * wins / N_TESTS; as = wins > 0 ? sum / wins : 0;
	printf("Test 01: winrate=%d%% av.steps=%d score=%d\n", wr, as, score);
	logfile << "#####\tTest 01: winrate="<<wr<<"% av.steps=" << as<<" score="<<score<<"\n";

	//Test 02 	one line O at wall
	logfile << "#####\tStarting Test 02...\n";
	wins = 0; sum = 0;
	for (int i = 0; i < N_TESTS; i++)
	{
		int ax = 1 + rand() % 6, ay = 1 + rand() % 6;
		if (rand() % 2 == 0) ax = (rand() % 2 == 0) ? 0 : 7;
		else ay = (rand() % 2 == 0) ? 0 : 7;
		int gx = ax, gy = ay;
		if (rand() % 2 == 0) while (gx == ax) gx = rand() % 8;
		else while (gy == ay) gy = rand() % 8;
		MakeLab(lab, ax, ay, ax, ay, gx, gy, 0);
		//test
		printf("Test 02: %d/%d             \n", i, N_TESTS);
		int res = GoTest(lab, PRINT_STEPS);
		if (res > 0) { wins++; sum += res; }
		else if(res==-2) StopAll();
		printf("\r\033[A");
	}
	score = 100 * (wins * STEPS_LIMIT - sum) / STEPS_LIMIT / N_TESTS;
	totalscore += score;
	wr = 100 * wins / N_TESTS; as = wins > 0 ? sum / wins : 0;
	printf("Test 02: winrate=%d%% av.steps=%d score=%d\n", wr, as, score);
	logfile << "#####\tTest 02: winrate="<<wr<<"% av.steps=" << as<<" score="<<score<<"\n";

	//Test 03 	all in corners
	logfile << "#####\tStarting Test 03...\n";
	wins = 0; sum = 0;
	for (int i = 0; i < N_TESTS; i++)
	{
		int ax = (rand() % 2 == 0)?0:7, ay = (rand() % 2 == 0) ? 0 : 7;
		int gx = ax, gy = ay;
		while (gx == ax && gy == ay)
		{
			gx = (rand() % 2 == 0) ? 0 : 7;
			gy = (rand() % 2 == 0) ? 0 : 7;
		}
		MakeLab(lab, ax, ay, ax, ay, gx, gy, 0);
		//test
		printf("Test 03: %d/%d             \n", i, N_TESTS);
		int res = GoTest(lab, PRINT_STEPS);
		if (res > 0) { wins++; sum += res; }
		else if(res==-2) StopAll();
		printf("\r\033[A");
	}
	score = 100 * (wins * STEPS_LIMIT - sum) / STEPS_LIMIT / N_TESTS;
	totalscore += score;
	wr = 100 * wins / N_TESTS; as = wins > 0 ? sum / wins : 0;
	printf("Test 03: winrate=%d%% av.steps=%d score=%d\n", wr, as, score);
	logfile << "#####\tTest 03: winrate="<<wr<<"% av.steps=" << as<<" score="<<score<<"\n";

	//Test 04 	at 1 line with 1 #
	logfile << "#####\tStarting Test 04...\n";
	wins = 0; sum = 0;
	for (int i = 0; i < N_TESTS; i++)
	{
		int ax = rand() % 8, ay = rand() % 8;
		int gx = ax, gy = ay;
		if (rand() % 2 == 0) while (abs(gx - ax) < 2) gx = rand() % 8;
		else while (abs(gy - ay) < 2) gy = rand() % 8;
		rx[0] = (gx + ax) / 2;
		ry[0] = (gy + ay) / 2;
		MakeLab(lab, ax, ay, ax, ay, gx, gy, 1, rx, ry);
		//test
		printf("Test 04: %d/%d             \n", i, N_TESTS);
		int res = GoTest(lab, PRINT_STEPS);
		if (res > 0) { wins++; sum += res; }
		else if(res==-2) StopAll();
		printf("\r\033[A");
	}
	score = 100 * (wins * STEPS_LIMIT - sum) / STEPS_LIMIT / N_TESTS;
	totalscore += score;
	wr = 100 * wins / N_TESTS; as = wins > 0 ? sum / wins : 0;
	printf("Test 04: winrate=%d%% av.steps=%d score=%d\n", wr, as, score);
	logfile << "#####\tTest 04: winrate="<<wr<<"% av.steps=" << as<<" score="<<score<<"\n";

	//Test 05 	not at 1 line with 2 #
	logfile << "#####\tStarting Test 05...\n";
	wins = 0; sum = 0;
	for (int i = 0; i < N_TESTS; i++)
	{
		int ax = rand() % 8, ay = rand() % 8;
		int gx = ax, gy = ay;
		while (gx == ax) gx = rand() % 8;
		while (gy == ay) gy = rand() % 8;
		rx[0] = gx;	ry[0] = ay;
		rx[1] = ax;	ry[1] = gy;
		MakeLab(lab, ax, ay, ax, ay, gx, gy, 2, rx, ry);
		//test
		printf("Test 05: %d/%d             \n", i, N_TESTS);
		int res = GoTest(lab, PRINT_STEPS);
		if (res > 0) { wins++; sum += res; }
		else if(res==-2) StopAll();
		printf("\r\033[A");
	}
	score = 100 * (wins * STEPS_LIMIT - sum) / STEPS_LIMIT / N_TESTS;
	totalscore += score;
	wr = 100 * wins / N_TESTS; as = wins > 0 ? sum / wins : 0;
	printf("Test 05: winrate=%d%% av.steps=%d score=%d\n", wr, as, score);
	logfile << "#####\tTest 05: winrate="<<wr<<"% av.steps=" << as<<" score="<<score<<"\n";

	//Tests 06-09 	not at 1 line with many #
	nr = 4;
	for (int k = 0; k < 4; k++)
	{
        logfile << "#####\tStarting Test 0"<<k+6<<"...\n";
		wins = 0;
		sum = 0;
		for (int i = 0; i < N_TESTS; i++)
		{
			int ax = rand() % 8, ay = rand() % 8;
			int gx = ax, gy = ay;
			while (gx == ax) gx = rand() % 8;
			while (gy == ay) gy = rand() % 8;
            bool done = false;
            while(!done)
			    done = MakeLab(lab, ax, ay, ax, ay, gx, gy, nr);
			//test
			printf("Test 0%d: %d/%d             \n", 6+k, i, N_TESTS);
			int res = GoTest(lab, PRINT_STEPS);
			if (res > 0) { wins++; sum += res; }
			else if(res==-2) StopAll();
			printf("\r\033[A");
		}
		score = 100 * (wins * STEPS_LIMIT - sum) / STEPS_LIMIT / N_TESTS;
		totalscore += score;
		wr = 100 * wins / N_TESTS; as = wins > 0 ? sum / wins : 0;
        printf("Test 0%d: winrate=%d%% av.steps=%d score=%d\n",6+k, wr, as, score);
        logfile << "#####\tTest 0"<<k+6<<": winrate="<<wr<<"% av.steps=" << as<<" score="<<score<<"\n";
		nr *= 2;
	}

	//Test 10 1 block line
	logfile << "#####\tStarting Test 10...\n";
	wins = 0; sum = 0;
	for (int i = 0; i < N_TESTS; i++)
	{
		int ax, ay, gx, gy;
		nr = 8;
		if(rand() % 2 == 0) //vert
		{
			ax = rand() % 8; gx = rand() % 8;
			if (rand() % 2 == 0) {ay = rand() % 2; gy = 6+rand() % 2;}
			else {ay = 6 + rand() % 2; gy = rand() % 2;}
			int y = 2 + rand() % 4;
			for (int ir = 0; ir < 8; ir++) { rx[ir] = ir; ry[ir] = y; }
		}
		else
		{
			ay = rand() % 8; gy = rand() % 8;
			if (rand() % 2 == 0) {ax = rand() % 2; gx = 6 + rand() % 2;}
			else {ax = 6 + rand() % 2; gx = rand() % 2;}
			int x = 2 + rand() % 4;
			for (int ir = 0; ir < 8; ir++) { rx[ir] = x; ry[ir] = ir; }
		}
		MakeLab(lab, ax, ay, ax, ay, gx, gy, nr, rx, ry);
		//test
		printf("Test 10: %d/%d             \n", i, N_TESTS);
		int res = GoTest(lab, PRINT_STEPS);
		if (res > 0) { wins++; sum += res; }
		else if(res==-2) StopAll();
		printf("\r\033[A");
	}
	score = 100 * (wins * STEPS_LIMIT - sum) / STEPS_LIMIT / N_TESTS;
	totalscore += score;
	wr = 100 * wins / N_TESTS; as = wins > 0 ? sum / wins : 0;
	printf("Test 10: winrate=%d%% av.steps=%d score=%d\n", wr, as, score);
	logfile << "#####\tTest 10: winrate="<<wr<<"% av.steps=" << as<<" score="<<score<<"\n";

	//Test 11 2 block lines
	logfile << "#####\tStarting Test 11...\n";
	wins = 0; sum = 0;
	for (int i = 0; i < N_TESTS; i++)
	{
		int ax, ay, gx, gy;
		nr = 16;
		if (rand() % 2 == 0) //vert
		{
			ax = rand() % 8; gx = rand() % 8;
			if (rand() % 2 == 0) { ay = rand() % 2; gy = 6 + rand() % 2; }
			else { ay = 6 + rand() % 2; gy = rand() % 2; }
			int y= 2 + rand() % 3;
			for (int ir = 0; ir < 8; ir++) { rx[ir * 2] = ir; ry[ir * 2] = y; rx[ir * 2 + 1] = ir; ry[ir * 2 + 1] = y + 1; }
		}
		else
		{
			ay = rand() % 8; gy = rand() % 8;
			if (rand() % 2 == 0) { ax = rand() % 2; gx = 6 + rand() % 2; }
			else { ax = 6 + rand() % 2; gx = rand() % 2; }
			int x = 2 + rand() % 3;
			for (int ir = 0; ir < 8; ir++) { rx[ir * 2] = x; ry[ir * 2] = ir; rx[ir * 2 + 1] = x+1; ry[ir * 2 + 1] = ir; }
		}
		MakeLab(lab, ax, ay, ax, ay, gx, gy, nr, rx, ry);
		//test
		printf("Test 11: %d/%d             \n", i, N_TESTS);
		int res = GoTest(lab, PRINT_STEPS);
		if (res > 0) { wins++; sum += res; }
		else if(res==-2) StopAll();
		printf("\r\033[A");
	}
	score = 100 * (wins * STEPS_LIMIT - sum) / STEPS_LIMIT / N_TESTS;
	totalscore += score;
	wr = 100 * wins / N_TESTS; as = wins > 0 ? sum / wins : 0;
	printf("Test 11: winrate=%d%% av.steps=%d score=%d\n", wr, as, score);
	logfile << "#####\tTest 11: winrate="<<wr<<"% av.steps=" << as<<" score="<<score<<"\n";

	//Test 12 3 block lines
    logfile << "#####\tStarting Test 12...\n";
	wins = 0; sum = 0;
	for (int i = 0; i < N_TESTS; i++)
	{
		int ax, ay, gx, gy;
		nr = 24;
		if (rand() % 2 == 0) //vert
		{
			ax = rand() % 8; gx = rand() % 8;
			if (rand() % 2 == 0) { ay = rand() % 2; gy = 6 + rand() % 2; }
			else { ay = 6 + rand() % 2; gy = rand() % 2; }
			int x = 2 + rand() % 2;
			for (int ir = 0; ir < 8; ir++)
				for (int ik = 0; ik < 3; ik++) { rx[ir * 3 + ik] = ir; ry[ir * 3 + ik] = x+ik; }
		}
		else
		{
			ay = rand() % 8; gy = rand() % 8;
			if (rand() % 2 == 0) { ax = rand() % 2; gx = 6 + rand() % 2; }
			else { ax = 6 + rand() % 2; gx = rand() % 2; }
			int y = 2 + rand() % 2;
			for (int ir = 0; ir < 8; ir++)
				for (int ik = 0; ik < 3; ik++) { rx[ir * 3 + ik] = y + ik; ry[ir * 3 + ik] = ir; }
		}
		MakeLab(lab, ax, ay, ax, ay, gx, gy, nr, rx, ry);
		//test
		printf("Test 12: %d/%d             \n", i, N_TESTS);
		int res = GoTest(lab, PRINT_STEPS);
		if (res > 0) { wins++; sum += res; }
		else if(res==-2) StopAll();
		printf("\r\033[A");
	}
	score = 100 * (wins * STEPS_LIMIT - sum) / STEPS_LIMIT / N_TESTS;
	totalscore += score;
	wr = 100 * wins / N_TESTS; as = wins > 0 ? sum / wins : 0;
	printf("Test 12: winrate=%d%% av.steps=%d score=%d\n", wr, as, score);
	logfile << "#####\tTest 12: winrate="<<wr<<"% av.steps=" << as<<" score="<<score<<"\n";

	//Test 13 4 block lines
	logfile << "#####\tStarting Test 13...\n";
	wins = 0; sum = 0;
	for (int i = 0; i < N_TESTS; i++)
	{
		int ax, ay, gx, gy;
		nr = 32;
		if (rand() % 2 == 0) //vert
		{
			ax = rand() % 8; gx = rand() % 8;
			if (rand() % 2 == 0) { ay = rand() % 2; gy = 6 + rand() % 2; }
			else { ay = 6 + rand() % 2; gy = rand() % 2; }
			for (int ir = 0; ir < 8; ir++)
				for (int ik = 0; ik < 4; ik++) { rx[ir * 4 + ik] = ir; ry[ir * 4 + ik] = 2+ik; }
		}
		else
		{
			ay = rand() % 8; gy = rand() % 8;
			if (rand() % 2 == 0) { ax = rand() % 2; gx = 6 + rand() % 2; }
			else { ax = 6 + rand() % 2; gx = rand() % 2; }
			for (int ir = 0; ir < 8; ir++)
				for (int ik = 0; ik < 4; ik++) { rx[ir * 4 + ik] = 2 + ik; ry[ir * 4 + ik] = ir; }
		}
		MakeLab(lab, ax, ay, ax, ay, gx, gy, nr, rx, ry);
		//test
		printf("Test 13: %d/%d             \n", i, N_TESTS);
		int res = GoTest(lab, PRINT_STEPS);
		if (res > 0) { wins++; sum += res; }
		else if(res==-2) StopAll();
		printf("\r\033[A");
	}
	score = 100 * (wins * STEPS_LIMIT - sum) / STEPS_LIMIT / N_TESTS;
	totalscore += score;
	wr = 100 * wins / N_TESTS; as = wins > 0 ? sum / wins : 0;
	printf("Test 13: winrate=%d%% av.steps=%d score=%d\n", wr, as, score);
	logfile << "#####\tTest 13: winrate="<<wr<<"% av.steps=" << as<<" score="<<score<<"\n";

	//end
	cout << "Total score = " << totalscore << endl;
	logfile << "#####\tAll done!\n";
        logfile.close();
	return 0;
}
