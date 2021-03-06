/* 
 * Author - Dustin Franklin (Nvidia Jetson Developer)
 * Modified by - Sahil Juneja, Kyle Stewart-Frantz
 *
 */

#include "ArmPlugin.h"
#include "PropPlugin.h"

#include "cudaMappedMemory.h"
#include "cudaPlanar.h"

#define PI 3.141592653589793238462643383279502884197169f

static float JOINT_MIN	    = -0.75f;
static float JOINT_MAX	    =  2.0f;
static float BASE_JOINT_MIN = -0.75f;
static float BASE_JOINT_MAX =  0.75f;

// Turn on velocity based control
#define VELOCITY_CONTROL 1
static float VELOCITY_MIN = -0.2f;
static float VELOCITY_MAX  = 0.2f;

// Define DQN API Settings

/*
 * The probability of choosing a random action will start at EPS_START
 * and will decay exponentially towards EPS_END. EPS_DECAY controls the 
 * rate of the decay.
*/
static bool  ALLOW_RANDOM	= true;
static bool  DEBUG_DQN		= false;
static float GAMMA			= 0.999f;
static float EPS_START		= 0.05f;		// TUNE was 0.9
static float EPS_END		= 0.05f;		// TUNE was 0.05
static int	 EPS_DECAY		= 200;

/*
/ DONE - Tune the following hyperparameters
/
*/

static int INPUT_WIDTH     = 64;
static int INPUT_HEIGHT    = 64;
static int INPUT_CHANNELS  = 3;
static const char *OPTIMIZER = "RMSprop";
static float LearningRate  = 0.0001f;	// TUNE was 0.1
static int REPLAY_MEMORY   = 10000;		// TUNE was 10000
static int BATCH_SIZE      = 32;
static bool USE_LSTM       = true;
static int LSTMSize        = 128;

static float maxLearningRate = 0.0;

/*
/ DONE - Define Reward Parameters
/
*/

#define REWARD_WIN  100.0f
#define REWARD_LOSS -100.0f
#define REWARD_MULTIPLIER 100.0f

// Define Object Names
#define WORLD_NAME "arm_world"
#define PROP_NAME  "tube"
#define GRIP_NAME  "gripper_middle"

// Define Collision Parameters
// Define Object Names
#define WORLD_NAME "arm_world"
#define PROP_NAME  "tube"
#define GRIP_NAME  "gripper_middle"

// Define Collision Parameters
#define COLLISION_GROUND		"ground_plane::link::collision"
#define COLLISION_ITEM			"tube::tube_link::tube_collision"
#define COLLISION_POINT_ARM		"arm::link2::collision2"
#define COLLISION_POINT  		"arm::gripperbase::gripper_link"
#define COLLISION_POINT_GRIP	"arm::gripper_base::gripper_collison"
#define COLLISION_POINT_GRIP2	"arm::gripper_middle::middle_collison"
#define COLLISION_POINT_GRIP3	"arm::gripper_left::left_gripper"
#define COLLISION_POINT_GRIP4	"arm::gripper_right::right_gripper"

// Animation Steps
#define ANIMATION_STEPS 1000

// which joint to check
#define CHECKGRIPPER true			// i.e. not the arm
#define ONLYCHECKGRIPPER false		// i.e. the arm or the gripper

#define WANTCAMERA1 1		// original
#define WANTCAMERA2 1		// overhead
#define WANTCAMERA3 0		// angled

#define MULTIPLOT_RUNS 0	// for stepping through learning rates

// Set Debug Mode
#define DEBUG  true
#define DEBUG2 false
#define DEBUG3 false

// Lock base rotation DOF (Add dof in header file if off)
#define LOCKBASE false

static const char *checkpointfile  = "../../../armplugin.cpt";
static const char *checkpointfile2 = "../../../armplugin2.cpt";
static const char *checkpointfile3 = "../../../armplugin3.cpt";

static const char *action_strs[] = {"No Action", "Base-", "Base+", "Joint 1-", "Joint 1+", "Joint 2-", "Joint 2+", "Joint 3-", "Joint 3+", "Oops-", "Oops+"};

typedef enum Colors {Red, BoldRed, Green, BoldGreen, Yellow, BoldYellow, Blue, BoldBlue, Magenta, BoldMagenta, Cyan, BoldCyan, Reset_} Colors_t;
static const char *colorTable[] = {
	"\033[0;31m",
	"\033[1;31m",
	"\033[0;32m",
	"\033[1;32m",
	"\033[0;33m",
	"\033[1;33m",
	"\033[0;34m",
	"\033[1;34m",
	"\033[0;35m",
	"\033[1;35m",
	"\033[0;36m",
	"\033[1;36m",
	"\033[0m"};

const char *xtermTitle = "\033]0;";
const char *xtermEndTitle = "\007";

static void color(Colors_t c) {
  std::cout << colorTable[c];
}

static void resetcolor() {
  std::cout << colorTable[Reset_];
}

namespace gazebo
{
 
// register this plugin with the simulator
GZ_REGISTER_MODEL_PLUGIN(ArmPlugin)

static float BoxDistance(const math::Box& a, const math::Box& b);

// constructor
ArmPlugin::ArmPlugin() : ModelPlugin(), 
	cameraNode(new gazebo::transport::Node()), 
	cameraNode2(new gazebo::transport::Node()), 
	cameraNode3(new gazebo::transport::Node()), 
	collisionNode(new gazebo::transport::Node())
{
	std::cout << colorTable[Green] << "ArmPlugin::ArmPlugin()" << colorTable[Reset_] << std::endl;

	for( uint32_t n=0; n < DOF; n++ )
		resetPos[n] = 0.0f;

	resetPos[1] = 0.25;

	for( uint32_t n=0; n < DOF; n++ )
	{
		ref[n] = resetPos[n]; //JOINT_MIN;
		vel[n] = 0.0f;
	}

	agent 	         = NULL;
	agent2 	         = NULL;
	agent3 	         = NULL;
	inputState       = NULL;
	inputState2      = NULL;
	inputState3      = NULL;
	inputBuffer[0]   = NULL;
	inputBuffer[1]   = NULL;
	inputBuffer2[0]  = NULL;
	inputBuffer2[1]  = NULL;
	inputBuffer3[0]  = NULL;
	inputBuffer3[1]  = NULL;
	inputBufferSize  = 0;
	inputRawWidth    = 0;
	inputRawHeight   = 0;
	actionJointDelta = 0.05f;	// TUNE was 0.1
	actionVelDelta   = 0.05f;	// TUNE was 0.1
	maxEpisodeLength = 400;		// TUNE was 100
	episodeFrames    = 0;

	newState         = false;
	newReward        = false;
	endEpisode       = false;
	rewardHistory    = 0.0f;
	rewardHistory2   = 0.0f;
	rewardHistory3   = 0.0f;
	testAnimation    = true;
	loopAnimation    = false;
	animationStep    = 0;
	lastGoalDistance = 0.0f;
	avgGoalDelta     = 0.0f;
	lastGoalPlaneDistance = 0.0f;
	avgGoalPlaneDelta= 0.0f;
	successfulGrabs  = 0;
	totalRuns        = 0;
	runHistoryIdx    = 0;
	runHistoryMax    = 0;

	// zero the run history buffer
	memset(runHistory, 0, sizeof(runHistory));

	// set the default reset position for each joint
	for( uint32_t n=0; n < DOF; n++ )
		resetPos[n] = 0.0f;

	resetPos[1] = 0.25;	 // make the arm canted forward a little

	// set the initial positions and velocities to the reset
	for( uint32_t n=0; n < DOF; n++ )
	{
		ref[n] = resetPos[n]; //JOINT_MIN;
		vel[n] = 0.0f;
	}

	// set the joint ranges
	for( uint32_t n=0; n < DOF; n++ )
	{
		jointRange[n][0] = JOINT_MIN;
		jointRange[n][1] = JOINT_MAX;
	}

	// if the base is freely rotating, set it's range separately
	if( !LOCKBASE )
	{
		jointRange[0][0] = BASE_JOINT_MIN;
		jointRange[0][1] = BASE_JOINT_MAX;
	}

	std::clog << "SuccessfulGrabs TotalRuns Accuracy LearningRate maxLearningRate LSTMSize Last100Accuracy" << std::endl << std::flush;

}


// Load
void ArmPlugin::Load(physics::ModelPtr _parent, sdf::ElementPtr /*_sdf*/) 
{
	std::cout << colorTable[Green] << "ArmPlugin::Load(" << _parent->GetName() << ")" << colorTable[Reset_] << std::endl;

	// Store the pointer to the model
	this->model = _parent;
	this->j2_controller = new physics::JointController(model);

	// Create our nodes for camera communication
	cameraNode->Init();
	cameraNode2->Init();
	cameraNode3->Init();
	/*
	/ DONE - Subscribe to camera topic
	/
	*/
	
	cameraSub = cameraNode->Subscribe("/gazebo/" WORLD_NAME "/camera/link/camera/image", &ArmPlugin::onCameraMsg, this);
	cameraSub2 = cameraNode2->Subscribe("/gazebo/" WORLD_NAME "/camera2/link/camera/image", &ArmPlugin::onCameraMsg2, this);
	cameraSub3 = cameraNode3->Subscribe("/gazebo/" WORLD_NAME "/camera3/link/camera/image", &ArmPlugin::onCameraMsg3, this);
	// Create our node for collision detection
	collisionNode->Init();
		
	/*
	/ DONE - Subscribe to prop collision topic
	/
	*/
	
	collisionSub = collisionNode->Subscribe("/gazebo/" WORLD_NAME "/" PROP_NAME "/tube_link/my_contact", &ArmPlugin::onCollisionMsg, this);

	// Listen to the update event. This event is broadcast every simulation iteration.
	this->updateConnection = event::Events::ConnectWorldUpdateBegin(boost::bind(&ArmPlugin::OnUpdate, this, _1));
}


// CreateAgent
bool ArmPlugin::createAgent()
{
	if( agent != NULL )
		delete agent;
	if( agent2 != NULL )
		delete agent2;
	if( agent3 != NULL )
		delete agent3;
			
	/*
	/ DONE - Create DQN Agent
	/
	*/
	
	std::cout << colorTable[Green] << "ArmPlugin::createAgent()" << colorTable[Reset_] << std::endl;
	agent = dqnAgent::Create(INPUT_WIDTH, INPUT_HEIGHT, INPUT_CHANNELS, DOF*2, 
						OPTIMIZER, LearningRate, REPLAY_MEMORY, BATCH_SIZE,
						GAMMA, EPS_START, EPS_END, EPS_DECAY, 
						USE_LSTM, LSTMSize, ALLOW_RANDOM, DEBUG_DQN);
	agent2 = dqnAgent::Create(INPUT_WIDTH, INPUT_HEIGHT, INPUT_CHANNELS, DOF*2, 
						OPTIMIZER, LearningRate, REPLAY_MEMORY, BATCH_SIZE,
						GAMMA, EPS_START, EPS_END, EPS_DECAY, 
						USE_LSTM, LSTMSize, ALLOW_RANDOM, DEBUG_DQN);
	agent3 = dqnAgent::Create(INPUT_WIDTH, INPUT_HEIGHT, INPUT_CHANNELS, DOF*2, 
						OPTIMIZER, LearningRate, REPLAY_MEMORY, BATCH_SIZE,
						GAMMA, EPS_START, EPS_END, EPS_DECAY, 
						USE_LSTM, LSTMSize, ALLOW_RANDOM, DEBUG_DQN);

	if( !agent || !agent2 || !agent3)
	{
		fprintf(stderr, "ArmPlugin - failed to create DQN agents\n");
		return false;
	}
    if (FILE *file = fopen(checkpointfile, "r")) {
		std::cout << colorTable[Green] << "ArmPlugin::LoadCheckpoint(" <<  checkpointfile << ")" << colorTable[Reset_] << std::endl;
		if (agent) agent->LoadCheckpoint(checkpointfile);
        fclose(file);
    }
    if (FILE *file = fopen(checkpointfile2, "r")) {
		std::cout << colorTable[Green] << "ArmPlugin::LoadCheckpoint(" <<  checkpointfile2 << ")" << colorTable[Reset_] << std::endl;
		if (agent2) agent2->LoadCheckpoint(checkpointfile2);
        fclose(file);
    }
    if (FILE *file = fopen(checkpointfile3, "r")) {
		std::cout << colorTable[Green] << "ArmPlugin::LoadCheckpoint(" <<  checkpointfile3 << ")" << colorTable[Reset_] << std::endl;
		if (agent3) agent3->LoadCheckpoint(checkpointfile3);
        fclose(file);
    }
	// Allocate the python tensor for passing the camera state

	if (!inputState) {
		inputState = Tensor::Alloc(INPUT_WIDTH, INPUT_HEIGHT, INPUT_CHANNELS);
	}
	if (!inputState2) {
		inputState2 = Tensor::Alloc(INPUT_WIDTH, INPUT_HEIGHT, INPUT_CHANNELS);
	}
	if (!inputState3) {
		inputState3 = Tensor::Alloc(INPUT_WIDTH, INPUT_HEIGHT, INPUT_CHANNELS);
	}
	
	if( !inputState || !inputState2 || !inputState3)
	{
		fprintf(stderr, "ArmPlugin - failed to allocate %ux%ux%u Tensor\n", INPUT_WIDTH, INPUT_HEIGHT, INPUT_CHANNELS);
		return false;
	}

	return true;
}



// onCameraMsgs
void ArmPlugin::onCameraMsg(ConstImageStampedPtr &_msg)
{
	// don't process the image if the agent hasn't been created yet
	if( !agent )
		return;

	// check the validity of the message contents
	if( !_msg )
	{
		fprintf(stderr, "ArmPlugin - recieved NULL message\n");
		return;
	}

	// retrieve image dimensions
	
	const int width  = _msg->image().width();
	const int height = _msg->image().height();
	const int bpp    = (_msg->image().step() / _msg->image().width()) * 8;	// bits per pixel
	const int size   = _msg->image().data().size();

	if( bpp != 24 )
	{
		fprintf(stderr, "ArmPlugin - expected 24BPP uchar3 image from camera, got %i\n", bpp);
		return;
	}

	// allocate temp image if necessary
	if( !inputBuffer[0] || size != inputBufferSize )
	{
		if( !cudaAllocMapped(&inputBuffer[0], &inputBuffer[1], size) )
		{
			fprintf(stderr, "ArmPlugin - cudaAllocMapped() failed to allocate %i bytes\n", size);
			return;
		}

		if(DEBUG3) {printf("ArmPlugin - allocated camera img buffer %ix%i  %i bpp  %i bytes\n", width, height, bpp, size);}
		
		inputBufferSize = size;
		inputRawWidth   = width;
		inputRawHeight  = height;
	}

	memcpy(inputBuffer[0], _msg->image().data().c_str(), inputBufferSize);
	newState = true;

	if(DEBUG3){printf("camera %i x %i  %i bpp  %i bytes\n", width, height, bpp, size);}

}

void ArmPlugin::onCameraMsg2(ConstImageStampedPtr &_msg)
{
	// don't process the image if the agent hasn't been created yet
	if( !agent2 )
		return;

	// check the validity of the message contents
	if( !_msg )
	{
		fprintf(stderr, "ArmPlugin - recieved NULL message\n");
		return;
	}

	// retrieve image dimensions
	
	const int width  = _msg->image().width();
	const int height = _msg->image().height();
	const int bpp    = (_msg->image().step() / _msg->image().width()) * 8;	// bits per pixel
	const int size   = _msg->image().data().size();

	if( bpp != 24 )
	{
		fprintf(stderr, "ArmPlugin - expected 24BPP uchar3 image from camera, got %i\n", bpp);
		return;
	}

	// allocate temp image if necessary
	if( !inputBuffer2[0] || size != inputBufferSize )
	{
		if( !cudaAllocMapped(&inputBuffer2[0], &inputBuffer2[1], size) )
		{
			fprintf(stderr, "ArmPlugin - cudaAllocMapped() failed to allocate %i bytes\n", size);
			return;
		}

		if(DEBUG3) {printf("ArmPlugin - allocated camera img buffer %ix%i  %i bpp  %i bytes\n", width, height, bpp, size);}
		
		inputBufferSize = size;
		inputRawWidth   = width;
		inputRawHeight  = height;
	}

	memcpy(inputBuffer2[0], _msg->image().data().c_str(), inputBufferSize);
	newState = true;

	if(DEBUG3){printf("camera2 %i x %i  %i bpp  %i bytes\n", width, height, bpp, size);}

}

void ArmPlugin::onCameraMsg3(ConstImageStampedPtr &_msg)
{
	// don't process the image if the agent hasn't been created yet
	if( !agent3 )
		return;

	// check the validity of the message contents
	if( !_msg )
	{
		fprintf(stderr, "ArmPlugin - recieved NULL message\n");
		return;
	}

	// retrieve image dimensions
	
	const int width  = _msg->image().width();
	const int height = _msg->image().height();
	const int bpp    = (_msg->image().step() / _msg->image().width()) * 8;	// bits per pixel
	const int size   = _msg->image().data().size();

	if( bpp != 24 )
	{
		fprintf(stderr, "ArmPlugin - expected 24BPP uchar3 image from camera, got %i\n", bpp);
		return;
	}

	// allocate temp image if necessary
	if( !inputBuffer3[0] || size != inputBufferSize )
	{
		if( !cudaAllocMapped(&inputBuffer3[0], &inputBuffer3[1], size) )
		{
			fprintf(stderr, "ArmPlugin - cudaAllocMapped() failed to allocate %i bytes\n", size);
			return;
		}

		if(DEBUG3) {printf("ArmPlugin - allocated camera img buffer %ix%i  %i bpp  %i bytes\n", width, height, bpp, size);}
		
		inputBufferSize = size;
		inputRawWidth   = width;
		inputRawHeight  = height;
	}

	memcpy(inputBuffer3[0], _msg->image().data().c_str(), inputBufferSize);
	newState = true;

	if(DEBUG3){printf("camera3 %i x %i  %i bpp  %i bytes\n", width, height, bpp, size);}

}

// onCollisionMsg
void ArmPlugin::onCollisionMsg(ConstContactsPtr &contacts)
{
	if(DEBUG3){printf("collision callback (%u contacts)\n", contacts->contact_size());}

	if( testAnimation )
		return;

	for (unsigned int i = 0; i < contacts->contact_size(); ++i)
	{
		if( strcmp(contacts->contact(i).collision2().c_str(), COLLISION_GROUND) == 0 )	// i.e. ground
			continue;

		if(DEBUG2){std::cout << colorTable[BoldCyan] << "Collision between[" << contacts->contact(i).collision1()
			     << "] and [" << contacts->contact(i).collision2() << "]" << colorTable[Reset_] << std::endl;}

		/*
		/ DONE - Check if there is collision between the arm and object, then issue learning reward
		/
		*/
		
		bool collisionCheckArm = (strcmp(contacts->contact(i).collision1().c_str(), COLLISION_ITEM) == 0) && (strcmp(contacts->contact(i).collision2().c_str(), COLLISION_POINT_ARM) == 0);	// i.e. tube with arm
		
		if (collisionCheckArm) {
			if (DEBUG) {std::cout << "\033[1;36mCollision between " << contacts->contact(i).collision1() << " and  " << contacts->contact(i).collision2() << "\033[0m" << std::endl;}
			if (ONLYCHECKGRIPPER&&CHECKGRIPPER) {	// then the arm collision is given a negative reward
				rewardHistory = REWARD_LOSS/10.0;
				newReward  = true;
				endEpisode = true;
			} else {
				rewardHistory = (1.0f - (float(episodeFrames) / float(maxEpisodeLength))) * REWARD_WIN;
				rewardHistory = REWARD_WIN;
				newReward  = true;
				endEpisode = true;
			}
		}
		bool collisionCheckGripper = (strcmp(contacts->contact(i).collision1().c_str(), COLLISION_ITEM) == 0) && (strcmp(contacts->contact(i).collision2().c_str(), COLLISION_POINT) == 0);	// i.e. tube with gripper
		if (collisionCheckGripper) {
			if (DEBUG) {std::cout << "\033[1;32mCollision between " << contacts->contact(i).collision1() << " and  " << contacts->contact(i).collision2() << "\033[0m" << std::endl;}
			rewardHistory = (1.0f - (float(episodeFrames) / float(maxEpisodeLength))) * REWARD_WIN*2;
			rewardHistory = REWARD_WIN;
			newReward  = true;
			endEpisode = true;
		}
		collisionCheckGripper = (strcmp(contacts->contact(i).collision1().c_str(), COLLISION_ITEM) == 0) && (strcmp(contacts->contact(i).collision2().c_str(), COLLISION_POINT_GRIP) == 0);	// i.e. tube with gripper
		if (collisionCheckGripper) {
			if (DEBUG) {std::cout << "\033[1;32mCollision between " << contacts->contact(i).collision1() << " and  " << contacts->contact(i).collision2() << "\033[0m" << std::endl;}
			rewardHistory = (1.0f - (float(episodeFrames) / float(maxEpisodeLength))) * REWARD_WIN*2;
			rewardHistory = REWARD_WIN;
			newReward  = true;
			endEpisode = true;
		}
		collisionCheckGripper = (strcmp(contacts->contact(i).collision1().c_str(), COLLISION_ITEM) == 0) && (strcmp(contacts->contact(i).collision2().c_str(), COLLISION_POINT_GRIP2) == 0);	// i.e. tube with gripper
		if (collisionCheckGripper) {
			if (DEBUG) {std::cout << "\033[1;32mCollision between " << contacts->contact(i).collision1() << " and  " << contacts->contact(i).collision2() << "\033[0m" << std::endl;}
			rewardHistory = (1.0f - (float(episodeFrames) / float(maxEpisodeLength))) * REWARD_WIN*2;
			rewardHistory = REWARD_WIN;
			newReward  = true;
			endEpisode = true;
		}
		collisionCheckGripper = (strcmp(contacts->contact(i).collision1().c_str(), COLLISION_ITEM) == 0) && (strcmp(contacts->contact(i).collision2().c_str(), COLLISION_POINT_GRIP3) == 0);	// i.e. tube with gripper
		if (collisionCheckGripper) {
			if (DEBUG) {std::cout << "\033[1;32mCollision between " << contacts->contact(i).collision1() << " and  " << contacts->contact(i).collision2() << "\033[0m" << std::endl;}
			rewardHistory = (1.0f - (float(episodeFrames) / float(maxEpisodeLength))) * REWARD_WIN*2;
			rewardHistory = REWARD_WIN;
			newReward  = true;
			endEpisode = true;
		}
		collisionCheckGripper = (strcmp(contacts->contact(i).collision1().c_str(), COLLISION_ITEM) == 0) && (strcmp(contacts->contact(i).collision2().c_str(), COLLISION_POINT_GRIP4) == 0);	// i.e. tube with gripper
		if (collisionCheckGripper) {
			if (DEBUG) {std::cout << "\033[1;32mCollision between " << contacts->contact(i).collision1() << " and  " << contacts->contact(i).collision2() << "\033[0m" << std::endl;}
			rewardHistory = (1.0f - (float(episodeFrames) / float(maxEpisodeLength))) * REWARD_WIN*2;
			rewardHistory = REWARD_WIN;
			newReward  = true;
			endEpisode = true;
		}
	}
}


// upon receiving a new frame, update the AI agent
// head on camera
bool ArmPlugin::updateAgent()
{
	// convert uchar3 input from camera to planar BGR
	if( CUDA_FAILED(cudaPackedToPlanarBGR((uchar3*)inputBuffer[1], inputRawWidth, inputRawHeight,
							         inputState->gpuPtr, INPUT_WIDTH, INPUT_HEIGHT)) )
	{
		fprintf(stderr, "ArmPlugin - failed to convert %zux%zu image to %ux%u planar BGR image\n",
			   inputRawWidth, inputRawHeight, INPUT_WIDTH, INPUT_HEIGHT);

		return false;
	}

	// select the next action
	int action = 0;
#if WANTCAMERA1
	if( !agent->NextAction(inputState, &action) )
	{
		fprintf(stderr, "ArmPlugin - failed to generate agent's next action\n");
		return false;
	}
#else
	return false;
#endif
	// make sure the selected action is in-bounds
	if( action < 0 || action >= DOF * 2 )
	{
		fprintf(stderr, "ArmPlugin - agent selected invalid action, %i\n", action);
		return false;
	}

	if(DEBUG2){printf("Agent selected action %s\n", action_strs[action]);}

	const int jointIdx = action / 2;
	if (WANTCAMERA2 && jointIdx == 0) {	// camera2 controls the base joint
		return false;
	}

#if VELOCITY_CONTROL
	// if the action is even, increase the joint position by the delta parameter
	// if the action is odd,  decrease the joint position by the delta parameter

		
	/*
	/ DONE - Increase or decrease the joint velocity based on whether the action is even or odd
	/
	*/
	
	float velocity = vel[jointIdx] + actionVelDelta * ((action % 2 == 0) ? 1.0f : -1.0f);

	if( velocity < VELOCITY_MIN )
		velocity = VELOCITY_MIN;

	if( velocity > VELOCITY_MAX )
		velocity = VELOCITY_MAX;

	vel[jointIdx] = velocity;
	
	for( uint32_t n=0; n < DOF; n++ )
	{
		ref[n] += vel[n];

		if( ref[n] < JOINT_MIN )
		{
			ref[n] = JOINT_MIN;
			vel[n] = 0.0f;
		}
		else if( ref[n] > JOINT_MAX )
		{
			ref[n] = JOINT_MAX;
			vel[n] = 0.0f;
		}
	}
#else
	
	/*
	/ DONE - Increase or decrease the joint position based on whether the action is even or odd
	/
	*/
	float joint = ref[jointIdx] + actionJointDelta * ((action % 2 == 0) ? 1.0f : -1.0f);

	// limit the joint to the specified range
	if( joint < JOINT_MIN )
		joint = JOINT_MIN;
	
	if( joint > JOINT_MAX )
		joint = JOINT_MAX;

	ref[jointIdx] = joint;

#endif

	return true;
}

// camera above
bool ArmPlugin::updateAgent2()
{
	// convert uchar3 input from camera to planar BGR
	if( CUDA_FAILED(cudaPackedToPlanarBGR((uchar3*)inputBuffer2[1], inputRawWidth, inputRawHeight,
							         inputState2->gpuPtr, INPUT_WIDTH, INPUT_HEIGHT)) )
	{
		fprintf(stderr, "ArmPlugin - failed to convert %zux%zu image to %ux%u planar BGR image\n",
			   inputRawWidth, inputRawHeight, INPUT_WIDTH, INPUT_HEIGHT);

		return false;
	}

	// select the next action
	int action = 0;
#if WANTCAMERA2
	if( !agent2->NextAction(inputState2, &action) )
	{
		fprintf(stderr, "ArmPlugin - failed to generate agent's next action\n");
		return false;
	}
#else
	return false;
#endif
	// make sure the selected action is in-bounds
	if( action < 0 || action >= DOF * 2 )
	{
		fprintf(stderr, "Agent selected invalid action, %i\n", action);
		return false;
	}

	if(DEBUG2){printf("Agent selected action %s\n", action_strs[action]);}

	const int jointIdx = action / 2;

	if (WANTCAMERA2 && jointIdx != 0) {	// camera2 controls the base joint
		return false;
	}
#if VELOCITY_CONTROL
	// if the action is even, increase the joint position by the delta parameter
	// if the action is odd,  decrease the joint position by the delta parameter

		
	/*
	/ DONE - Increase or decrease the joint velocity based on whether the action is even or odd
	/
	*/
	
	float velocity = vel[jointIdx] + actionVelDelta * ((action % 2 == 0) ? 1.0f : -1.0f);

	if( velocity < VELOCITY_MIN )
		velocity = VELOCITY_MIN;

	if( velocity > VELOCITY_MAX )
		velocity = VELOCITY_MAX;

	vel[jointIdx] = velocity;
	
	for( uint32_t n=0; n < DOF; n++ )
	{
		ref[n] += vel[n];

		if( ref[n] < JOINT_MIN )
		{
			ref[n] = JOINT_MIN;
			vel[n] = 0.0f;
		}
		else if( ref[n] > JOINT_MAX )
		{
			ref[n] = JOINT_MAX;
			vel[n] = 0.0f;
		}
	}
#else
	
	/*
	/ DONE - Increase or decrease the joint position based on whether the action is even or odd
	/
	*/
	float joint = ref[jointIdx] + actionJointDelta * ((action % 2 == 0) ? 1.0f : -1.0f);

	// limit the joint to the specified range
	if( joint < JOINT_MIN )
		joint = JOINT_MIN;
	
	if( joint > JOINT_MAX )
		joint = JOINT_MAX;

	ref[jointIdx] = joint;

#endif

	return true;
}

// angle shot
bool ArmPlugin::updateAgent3()
{
	// convert uchar3 input from camera to planar BGR
	if( CUDA_FAILED(cudaPackedToPlanarBGR((uchar3*)inputBuffer3[1], inputRawWidth, inputRawHeight,
							         inputState3->gpuPtr, INPUT_WIDTH, INPUT_HEIGHT)) )
	{
		fprintf(stderr, "ArmPlugin - failed to convert %zux%zu image to %ux%u planar BGR image\n",
			   inputRawWidth, inputRawHeight, INPUT_WIDTH, INPUT_HEIGHT);

		return false;
	}

	// select the next action
	int action = 0;
#if WANTCAMERA3
	if( !agent3->NextAction(inputState3, &action) )
	{
		fprintf(stderr, "ArmPlugin - failed to generate agent's next action\n");
		return false;
	}
#else
	return false;
#endif
	// make sure the selected action is in-bounds
	if( action < 0 || action >= DOF * 2 )
	{
		fprintf(stderr, "Agent selected invalid action, %i\n", action);
		return false;
	}

	const int jointIdx = action / 2;

	if (WANTCAMERA2 && jointIdx == 0) {	// camera2 controls the base joint
		return false;
	}

	if(DEBUG2){printf("Agent selected action %s\n", action_strs[action]);}

#if VELOCITY_CONTROL
	// if the action is even, increase the joint position by the delta parameter
	// if the action is odd,  decrease the joint position by the delta parameter

		
	/*
	/ DONE - Increase or decrease the joint velocity based on whether the action is even or odd
	/
	*/
	
	float velocity = vel[jointIdx] + actionVelDelta * ((action % 2 == 0) ? 1.0f : -1.0f);

	if( velocity < VELOCITY_MIN )
		velocity = VELOCITY_MIN;

	if( velocity > VELOCITY_MAX )
		velocity = VELOCITY_MAX;

	vel[jointIdx] = velocity;
	
	for( uint32_t n=0; n < DOF; n++ )
	{
		ref[n] += vel[n];

		if( ref[n] < JOINT_MIN )
		{
			ref[n] = JOINT_MIN;
			vel[n] = 0.0f;
		}
		else if( ref[n] > JOINT_MAX )
		{
			ref[n] = JOINT_MAX;
			vel[n] = 0.0f;
		}
	}
#else
	
	/*
	/ DONE - Increase or decrease the joint position based on whether the action is even or odd
	/
	*/
	float joint = ref[jointIdx] + actionJointDelta * ((action % 2 == 0) ? 1.0f : -1.0f);

	// limit the joint to the specified range
	if( joint < JOINT_MIN )
		joint = JOINT_MIN;
	
	if( joint > JOINT_MAX )
		joint = JOINT_MAX;

	ref[jointIdx] = joint;

#endif

	return true;
}

// update joint reference positions, returns true if positions have been modified
bool ArmPlugin::updateJoints()
{
	if( testAnimation )	// test sequence
	{
		const float step = (JOINT_MAX - JOINT_MIN) * (float(1.0f) / float(ANIMATION_STEPS));

#if 0
		// range of motion
		if( animationStep < ANIMATION_STEPS )
		{
			animationStep++;
			if (DEBUG3) printf("animation step %u\n", animationStep);

			for( uint32_t n=0; n < DOF; n++ )
				ref[n] = JOINT_MIN + step * float(animationStep);
		}
		else if( animationStep < ANIMATION_STEPS * 2 )
		{			
			animationStep++;
			if (DEBUG3) printf("animation step %u\n", animationStep);

			for( uint32_t n=0; n < DOF; n++ )
				ref[n] = JOINT_MAX - step * float(animationStep-ANIMATION_STEPS);
		}
		else
		{
			animationStep = 0;

		}

#else
		// return to base position
		for( uint32_t n=0; n < DOF; n++ )
		{
			
			if( ref[n] < resetPos[n] )
				ref[n] += step;
			else if( ref[n] > resetPos[n] )
				ref[n] -= step;

			if( ref[n] < JOINT_MIN )
				ref[n] = JOINT_MIN;
			else if( ref[n] > JOINT_MAX )
				ref[n] = JOINT_MAX;
			
		}

		animationStep++;
#endif

		// reset and loop the animation
		if( animationStep > ANIMATION_STEPS )
		{
			animationStep = 0;
			
			if( !loopAnimation )
				testAnimation = false;
		}
		else if( animationStep == ANIMATION_STEPS / 2 )
		{	
			//ResetPropDynamics();
			RandomizeProps();
		}

		return true;
	}

	else if( newState && agent != NULL )
	{
		// update the AI agent when new camera frame is ready
		episodeFrames++;

		if(DEBUG2){printf("episode frame = %i\n", episodeFrames);}

		// reset camera ready flag
		newState = false;

		bool updated1 = (agent?updateAgent():false);
		bool updated2 = (agent2?updateAgent2():false);
		bool updated3 = (agent3?updateAgent3():false);
		return updated1||updated2||updated3;
	}
	return false;
}


// get the servo center for a particular degree of freedom
float ArmPlugin::resetPosition( uint32_t dof )
{
	return resetPos[dof];
}

// compute the distance between two bounding boxes
static inline float BoxDistance(const math::Box& a, const math::Box& b)
{
	float sqrDist = 0;

	if( b.max.x < a.min.x )
	{
		float d = b.max.x - a.min.x;
		sqrDist += d * d;
	}
	else if( b.min.x > a.max.x )
	{
		float d = b.min.x - a.max.x;
		sqrDist += d * d;
	}

	if( b.max.y < a.min.y )
	{
		float d = b.max.y - a.min.y;
		sqrDist += d * d;
	}
	else if( b.min.y > a.max.y )
	{
		float d = b.min.y - a.max.y;
		sqrDist += d * d;
	}

	if( b.max.z < a.min.z )
	{
		float d = b.max.z - a.min.z;
		sqrDist += d * d;
	}
	else if( b.min.z > a.max.z )
	{
		float d = b.min.z - a.max.z;
		sqrDist += d * d;
	}
	
	return sqrtf(sqrDist);
}

// compute the distance between two bounding boxes
static inline float BoxZDistance(const math::Box& a, const math::Box& b)
{
	float sqrDist = 0;

	if( b.max.z < a.min.z )
	{
		float d = b.max.z - a.min.z;
		sqrDist += d * d;
	}
	else if( b.min.z > a.max.z )
	{
		float d = b.min.z - a.max.z;
		sqrDist += d * d;
	}
	
	return sqrtf(sqrDist);
}

static inline float BoxXYDistance(const math::Box& a, const math::Box& b)
{
	float sqrDist = 0;

	if( b.max.x < a.min.x )
	{
		float d = b.max.x - a.min.x;
		sqrDist += d * d;
	}
	else if( b.min.x > a.max.x )
	{
		float d = b.min.x - a.max.x;
		sqrDist += d * d;
	}

	if( b.max.y < a.min.y )
	{
		float d = b.max.y - a.min.y;
		sqrDist += d * d;
	}
	else if( b.min.y > a.max.y )
	{
		float d = b.min.y - a.max.y;
		sqrDist += d * d;
	}

	return sqrtf(sqrDist);
}


// called by the world update start event
void ArmPlugin::OnUpdate(const common::UpdateInfo& updateInfo)
{
	// deferred loading of the agent (this is to prevent Gazebo black/frozen display)
	if( !agent && updateInfo.simTime.Float() > 1.5f )
	{
		if( !createAgent() )
			return;
	}
#if MULTIPLOT_RUNS
	if ((endEpisode && totalRuns >= 100) || (totalRuns >= 102)) {
		// tune a parameter and create a new agent
		maxLearningRate = (LearningRate>maxLearningRate?LearningRate:maxLearningRate);
		LearningRate += 0.05;
		createAgent();
		std::cout << colorTable[BoldBlue] << "*** Restarting with learning rate " <<  LearningRate << " maxLearningRate = " << maxLearningRate << " accuracy = " << (float(successfulGrabs)/float(totalRuns)) << " LSTM Size = " << LSTMSize << colorTable[Reset_] << std::endl;
		std::clog << std::endl << std::endl << std::flush;	// spacer
		std::clog << "SuccessfulGrabs TotalRuns Accuracy LearningRate maxLearningRate LSTMSize Last100Accuracy" << std::endl << std::flush;
		totalRuns = 0;
		successfulGrabs = 0;
		return;
	}
#endif

	// verify that the agent is loaded
	if( !agent )
		return;

	// determine if we have new camera state and need to update the agent
	const bool hadNewState = newState && !testAnimation;
	float distGround = 0.0;
	float distGoal = 0.0;
	float distGoalPlane = 0.0;

	// update the robot positions with vision/DQN
	if( updateJoints() )
	{
		double angle(1);

#if LOCKBASE
		j2_controller->SetJointPosition(this->model->GetJoint("base"), 	0);
		j2_controller->SetJointPosition(this->model->GetJoint("joint1"),  ref[0]);
		j2_controller->SetJointPosition(this->model->GetJoint("joint2"),  ref[1]);

#else
		j2_controller->SetJointPosition(this->model->GetJoint("base"), 	 ref[0]); 
		j2_controller->SetJointPosition(this->model->GetJoint("joint1"),  ref[1]);
		j2_controller->SetJointPosition(this->model->GetJoint("joint2"),  ref[2]);
#endif
	}

	// episode timeout
	if( maxEpisodeLength > 0 && episodeFrames > maxEpisodeLength )
	{
		std::cout << colorTable[BoldYellow] << maxEpisodeLength << " frames exceeded, EOE" << colorTable[Reset_] << std::endl;
		rewardHistory = REWARD_LOSS;
		newReward     = true;
		endEpisode    = true;
	}

	// if an EOE reward hasn't already been issued, compute an intermediary reward
	if( hadNewState && !newReward )
	{
		PropPlugin* prop = GetPropByName(PROP_NAME);

		if( !prop )
		{
			fprintf(stderr, "ArmPlugin - failed to find Prop '%s'\n", PROP_NAME);
			return;
		}

		// get the bounding box for the prop object
		const math::Box& propBBox = prop->model->GetBoundingBox();
		physics::LinkPtr gripper  = model->GetLink(GRIP_NAME);

		if( !gripper )
		{
			fprintf(stderr, "ArmPlugin - failed to find gripper '%s'\n", PROP_NAME);
			return;
		}

		// get the bounding box for the gripper		
		const math::Box& gripBBox = gripper->GetBoundingBox();
		const float groundContact = -0.5f;
		distGround = BoxZDistance(gripBBox, propBBox);
		
		distGoal = BoxDistance(gripBBox, propBBox); // compute the reward from distance to the goal
		distGoalPlane = BoxXYDistance(gripBBox, propBBox); // XY plane distance for overhead camera
		/*
		/ DONE - set appropriate Reward for robot hitting the ground.
		/
		*/
			
		if(distGround <= groundContact)
		{				
			rewardHistory = REWARD_LOSS;
			newReward     = true;
			endEpisode    = true;
			std::cout << colorTable[BoldYellow] << "Ground Contact, distance from goal: " << distGoal << ", REWARD: " << rewardHistory << " EOE" << " dist ground = " << distGround << colorTable[Reset_] << std::endl;
		}		
		else
		{

			/*
			/ DONE - Issue an interim reward based on the distance to the object
			/
			*/ 
		
			if(DEBUG2){printf("distance(('%s', '%s'), 'ground') = %f %f\n", gripper->GetName().c_str(), prop->model->GetName().c_str(), distGoal, distGround);}

			if( episodeFrames > 1 && !endEpisode)
			{
				const float distDelta  = lastGoalDistance - distGoal;
				const float alpha = 0.1;	// 10% current dist, 90% historical average

				// compute the smoothed moving average of the delta of the distance to the goal
				avgGoalDelta  = (distDelta * alpha) + (avgGoalDelta * (1 - alpha));
				// tanh returns values from -1.0 to 1.0, which is then adjusted by the reward amount
				rewardHistory = tanh(avgGoalDelta)*REWARD_WIN*REWARD_MULTIPLIER;
				newReward     = true;	

				const float distDeltaPlane  = lastGoalPlaneDistance - distGoalPlane;

				// compute the smoothed moving average of the delta of the distance to the goal
				avgGoalPlaneDelta  = (distDeltaPlane * alpha) + (avgGoalPlaneDelta * (1 - alpha));
				// tanh returns values from -1.0 to 1.0, which is then adjusted by the reward amount
				rewardHistory3 = tanh(avgGoalPlaneDelta)*REWARD_WIN*REWARD_MULTIPLIER;
				newReward     = true;	
			}

			lastGoalDistance = distGoal;
			lastGoalPlaneDistance = distGoalPlane;
		}
	}

	// issue rewards and train DQN
	if( newReward && agent != NULL )
	{
		if(DEBUG2){printf("\033[1;32mIssuing reward %f, %s %s\033[0m\n", rewardHistory, (endEpisode ? "EOE" : ""), (rewardHistory > 0.1f) ? "POS+" :(rewardHistory > 0.0f) ? "POS" : (rewardHistory < 0.0f) ? "    NEG" : "       ZERO");}

		const uint32_t RUN_HISTORY = sizeof(runHistory);
		uint32_t historyWins = 0;

		// send reward to DQN
		if (agent)  agent->NextReward(rewardHistory, endEpisode);
		if (agent2) agent2->NextReward(rewardHistory2, endEpisode);
		if (agent3) agent3->NextReward(rewardHistory3, endEpisode);

		// reset reward indicator
		newReward = false;

		// reset for next episode
		if ( endEpisode )
		{
			testAnimation    = true;	// reset the robot to base position
			loopAnimation    = false;
			endEpisode       = false;
			episodeFrames    = 0;
			lastGoalDistance = 0.0f;
			avgGoalDelta     = 0.0f;
			lastGoalPlaneDistance = 0.0f;
			avgGoalPlaneDelta     = 0.0f;

			// track the number of wins and agent accuracy
			if( rewardHistory >= REWARD_WIN ) {
				runHistory[runHistoryIdx] = true;
				successfulGrabs++;
			} else {
				runHistory[runHistoryIdx] = false;
			}

			runHistoryIdx = (runHistoryIdx + 1) % RUN_HISTORY;

			totalRuns++;
			printf("Current Accuracy: %0.2f (%03u of %03u)  (reward=%.2f %s) ", float(successfulGrabs)/float(totalRuns), successfulGrabs, totalRuns, rewardHistory, (rewardHistory >= REWARD_WIN ? "\033[0;32mWIN\033[0m" : "\033[0;31mLOSS\033[0m"));

			if( totalRuns >= RUN_HISTORY )
			{

				for( uint32_t n=0; n < RUN_HISTORY; n++ )
				{
					if( runHistory[n] )
						historyWins++;
				}

				if( historyWins > runHistoryMax )
					runHistoryMax = historyWins;

				printf("%02u of last %u  (%0.2f)  (max=%0.2f)", historyWins, RUN_HISTORY, float(historyWins)/float(RUN_HISTORY), float(runHistoryMax)/float(RUN_HISTORY));
			}

			printf("\n");

			std::clog <<  
				successfulGrabs << " " << totalRuns << " " << 
				(float(successfulGrabs)/float(totalRuns)) << " " <<
				LearningRate << " " << maxLearningRate << " " << 
				LSTMSize << " " << (float(historyWins)/float(RUN_HISTORY)) <<
				std::endl << std::flush;

			for( uint32_t n=0; n < DOF; n++ )
				vel[n] = 0.0f;
		}
		if (episodeFrames > 1) {	// xterm title bar
			printf("%sAcu: %0.2f %03u/%03u,Rew=%.2f Delta=%.2f %03u/%03u%s", xtermTitle, float(successfulGrabs)/float(totalRuns), successfulGrabs, totalRuns, rewardHistory, avgGoalDelta, historyWins, RUN_HISTORY, xtermEndTitle);
		}
		if (totalRuns > 0 && (totalRuns%100) == 0) {	// save a checkpoint every 100 runs
			if (agent) agent->SaveCheckpoint(checkpointfile);
			if (agent2) agent2->SaveCheckpoint(checkpointfile2);
			if (agent3) agent3->SaveCheckpoint(checkpointfile3);
		}
	}
}

}	// namespace
