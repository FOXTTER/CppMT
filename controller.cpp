#include "controller.h"
#include <stdio.h>
#include <ros/ros.h>
#include <image_transport/image_transport.h>
#include <cv_bridge/cv_bridge.h>
#include <sensor_msgs/image_encodings.h>
#include <std_msgs/Empty.h>
#include <ardrone_autonomy/Navdata.h>
#include <geometry_msgs/Twist.h>
#include <cv.h>
#include <highgui.h>
#define KP_X -0.16
#define KP_Y 0.00016
#define KP_ROT 0.5
#define KP_Z 0.001
#define KI_X 0.000
#define KI_Y 0.0000
#define KI_ROT 0.0
#define KI_Z 0.0000
#define KD_X -2
#define KD_Y 0.000075
#define KD_ROT 3
#define KD_Z 1
#define X 0
#define Y 1
#define ROT 2
#define Z 3
#define LANDED 2
//Husk at dobbelttjek disse værdier
#define GAMMA_X 40 //grader
#define GAMMA_Y 64
#define PIXEL_DIST_X 180 //pixels
#define PIXEL_DIST_Y 320
#define FOCAL_LENGTH_X 607.7
#define FOCAL_LENGTH_Y 554

#define FILTER_WEIGHT 0.5
using namespace cv;




namespace controller
{

	void Controller::nav_callback(const ardrone_autonomy::Navdata& msg_in)
	{
		//Take in navdata from ardrone
		msg_in_global = msg_in;
	}
	void Controller::logData()
	{
	  FILE* pFile = fopen("quadlog.txt", "a");
	  fprintf(pFile, "%g,%g,%g,%g,%g,%g,%g\n",(double)ros::Time::now().toSec()-start_time,measured[X],measured[Y],measured[Z],output[X],output[Y],output[Z]);
	  fclose(pFile);
	}

	struct Foo
	{
	  class Request
	  {
	  };
	
	  class Response
	  {
	  };
	
	  Request request;
	  Response response;
	}; 
	Controller::Controller()
	:previous_error(4,0.0)
	,error(4,0.0)
	,output(4,0.0)
	,derivative(4,0.0)
	,integral(4,0.0)
	,target(4,0.0)
	,Kp(4,0.0)
	,Ki(4,0.0)
	,Kd(4,0.0)
	,measured(4,0.0)
	{
		//Kp[X] = KP_X;
		//Kp[Y] = KP_Y;
		//Kp[ROT] = KP_ROT;
		//Kp[Z] = KP_Z;
		//Ki[X] = KI_X;
		//Ki[Y] = KI_Y;
		//Ki[ROT] = KI_ROT;
		//Ki[Z] = KI_Z;
		nav_sub = node.subscribe("/ardrone/navdata", 1, &Controller::nav_callback,this);
		pub_twist = node.advertise<geometry_msgs::Twist>("/cmd_vel", 1); /* Message queue length is just 1 */
		pub_empty_takeoff = node.advertise<std_msgs::Empty>("/ardrone/takeoff", 1); /* Message queue length is just 1 */
		pub_empty_land = node.advertise<std_msgs::Empty>("/ardrone/land", 1); /* Message queue length is just 1 */
		pub_empty_reset = node.advertise<std_msgs::Empty>("/ardrone/reset", 1); /* Message queue length is just 1 */
		pseudo_hover_msg.linear.x = 0;
		pseudo_hover_msg.linear.y = 0;
		pseudo_hover_msg.linear.z = 0;
		pseudo_hover_msg.angular.x = 1;
		pseudo_hover_msg.angular.y = 1;
		pseudo_hover_msg.angular.z = 0;
		
		//client = node.serviceClient<std_msgs::Empty>("/ardrone/flattrim");
	}


	void Controller::takeoff(){
		pub_empty_takeoff.publish(emp_msg);
	}
	void Controller::calibrate(){
		//Controller::Foo foo;
		//ros::service::call("/ardrone/flattrim",foo);
	}
	void Controller::wait(double tid){
		double time_start=(double)ros::Time::now().toSec();
 		while (ros::ok() && ((double)ros::Time::now().toSec()< time_start+tid)){
 			ros::spinOnce();
 		}
	}
    void Controller::land(){
        pub_empty_land.publish(emp_msg);
    }
    
    void Controller::reset(){
        pub_empty_reset.publish(emp_msg);
    }

    void Controller::init(){
        while (msg_in_global.state != LANDED) {
        	ros::spinOnce();
            Controller::reset();
            ROS_INFO("State: %d",msg_in_global.state);
        }
        start_time = (double)ros::Time::now().toSec();
    }
    //Altitude in millimeters
    void Controller::elevate(double altitude){ 
    	twist_msg.linear.z = 0.5;
    	pub_twist.publish(twist_msg);
    	while(msg_in_global.altd < altitude){
    		ros::spinOnce();
    	}
    	twist_msg.linear.z = 0;
    	Controller::auto_hover();
    }

    void Controller::setTargetRot(){
    	target[ROT] = 0;
    }
    void Controller::setTargetRect(Rect rect){
    	target[X] = sqrt((PIXEL_DIST_X*PIXEL_DIST_Y)/(rect.width*rect.height));
    }

    double Controller::getPosX(int pixErrorX){
        double alphaX = ((msg_in_global.rotY*3.14)/180); // SKAL HENTES FRA QUADCOPTEREN
        double betaX = -atan(tan(GAMMA_X/2)*(pixErrorX)/PIXEL_DIST_X);
        double height = ((double)msg_in_global.altd)/1000; //HØJDEMÅLING FRA ULTRALYD
        //Negative sign to get drone position and not tracket object
        return -(height * tan(alphaX+betaX));
    }
    double Controller::getPosY(int pixErrorY){
        double alphaY = ((msg_in_global.rotX*3.14)/180); // SKAL HENTES FRA QUADCOPTEREN
        double betaY = -atan(tan(GAMMA_Y/2)*(pixErrorY)/PIXEL_DIST_Y);
        double height = ((double)msg_in_global.altd)/1000; //HØJDEMÅLING FRA ULTRALYD
        //Negative sign to get drone position and not tracket object
        return -(height * tan(alphaY+betaY));
    }

    void Controller::auto_hover(){
    	pub_twist.publish(twist_msg_hover);
    }
    void Controller::pseudo_hover(){
    	pub_twist.publish(pseudo_hover_msg);
    }
    //Filtered data
	void Controller::update_state(Point2f center, Rect rect)
	{
		//measured[X] = measured[X]+ FILTER_WEIGHT*(getPosX(center.y-PIXEL_DIST_X)-measured[X]); 
		//measured[Y] = measured[Y]+ FILTER_WEIGHT*(getPosY(center.x-PIXEL_DIST_Y)-measured[Y]);
		//measured[X] = getPosX(center.y-PIXEL_DIST_X);
		//measured[Y] = getPosY(center.x-PIXEL_DIST_Y);
		//Kun til front kamera
		measured[ROT] = atan(((center.x-PIXEL_DIST_Y)*tan(1.6/2))/(PIXEL_DIST_Y/2));
		measured[X] = sqrt((PIXEL_DIST_X*PIXEL_DIST_Y)/(rect.width*rect.height));
		measured[Y] = measured[Y]+ FILTER_WEIGHT*((center.x-PIXEL_DIST_Y)-measured[Y]);
		measured[Z] = measured[Z]+ FILTER_WEIGHT*(((center.y-PIXEL_DIST_X)+sin(msg_in_global.rotY*3.14/180)*FOCAL_LENGTH_X)-measured[Z]); 
		//ROS_INFO("DEBUG: %g",measured[Y]);
		//Den fancy måde
		//measured[Y] = center.x/PIXEL_DIST_Y;
		//measured[Z] = center.y/PIXEL_DIST_X;
		//measured[X] = sqrt((PIXEL_DIST_X*PIXEL_DIST_Y)/(rect.width*rect.height));
	}

	void Controller::control(double dt)
	 {	//Original controller
	 	for(int i = 0; i < 4; i++)
	 	{
      		error[i] = target[i] - measured[i];
      		if (output[i] < 1 && output[i] > -1)
      		{
      			integral[i] = integral[i] + error[i] * dt;
      		}
      		derivative[i] = (error[i]-previous_error[i])/dt;
      		output[i] = Kp[i]*error[i] + Ki[i] * integral[i] + Kd[i] * derivative[i];
      		previous_error[i] = error[i];
    	}

      	//Eksperimentiel
      	/*
      	error[X] = (measured[X] - target[X])*sqrt(0.006)*sqrt((alfa_u*alfa_v)/(PIXEL_DIST_X*PIXEL_DIST_Y));
      	error[Y] = (measured[Y] - target[Y])*2*PIXEL_DIST_X/alfa_u;
      	error[ROT] = (measured)
      	for(int i = 0; i < 4; i++)
	 	{
      		if (output[i] < 1 && output[i] > -1)
      		{
      			integral[i] = integral[i] + error[i] * dt;
      		}
      		derivative[i] = (error[i]-previous_error[i])/dt;
      		output[i] = Kp[i]*error[i] + Ki[i] * integral[i] + Kd[i] * derivative[i];
      		previous_error[i] = error[i];
    	}*/
    	/*if (output[X] > 0.1)
    	{
    		output[X] = 0.1;
    	}
    	if (output[X] < -0.1)
    	{
    		output[X] = -0.1;
    	}
    	if (output[Y] > 0.1)
    	{
    		output[Y] = 0.1;
    	}
    	if (output[Y] < -0.1)
    	{
    		output[Y] = -0.1;
    	}
    	*/

    	twist_msg.angular.z= output[ROT];
    	twist_msg.linear.x = output[X];
    	twist_msg.linear.y = output[Y];
    	twist_msg.linear.z = output[Z];


    	pub_twist.publish(twist_msg);

    	ROS_INFO("Measured pos = (%g,%g)",measured[X],measured[Y]);
    	ROS_INFO("Output x: %g", output[X]);
  		ROS_INFO("Output y: %g", output[Y]);
  		ROS_INFO("Output z: %g", output[Z]);
  		ROS_INFO("Output r: %g", output[ROT]);
  		//ROS_INFO("Integral x: %g", Ki[X]*integral[X]);
  		//ROS_INFO("Integral y: %g", Ki[Y]*integral[Y]);

	 }
}