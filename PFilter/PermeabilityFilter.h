#pragma once

#define _USE_MATH_DEFINES
#include <opencv2/opencv.hpp>
#include <cmath>
#include <assert.h>

#include "globals.h"
#include "flowIO.h"
#include "ImageIOpfm.h"

using namespace cv;
using namespace std;


const Vec2i kPOSITION_INVALID = Vec2i(-1, -1);
const float kMOVEMENT_UNKNOWN = 1e10;
const Vec2f kFLOW_UNKNOWN = Vec2f(kMOVEMENT_UNKNOWN,kMOVEMENT_UNKNOWN);


// Transforms a relative flow R into an absolute flow A, checking for margins
// Ax = X + Rx , Ay = Y + Ry
// if either
//   - Ax is outside [0,w] or
//   - Ay outside [0,y]
// it returns kPOSITION_INVALID
inline Vec2i getAbsoluteFlow(int x, int y, const Vec2f& flow, int h, int w)
{
    Vec2i result(cvRound(y + flow[1]), cvRound(x + flow[0]));
    if(result[0] >= 0 && result[0] < h && result[1] >= 0 && result[1] < w)
        return result;
    else
        return kPOSITION_INVALID;
}

// Computes the normalized confidence map C between forward flow F and backward flow B
// First, the disntance D at every position (X,Y) is computed :
//     D(X,Y) = ||F(X,Y) - B(X + Fx(X,Y),y + Fy(X,Y))||
// Then, normalized confidence map C is computed :
//     C' = 1 - D / max(D)
inline Mat1f getFlowConfidence(Mat2f forward_flow, Mat2f backward_flow)
{
    Mat1f distances = Mat1f(forward_flow.rows, forward_flow.cols, -1);

    int h = forward_flow.rows;
    int w = forward_flow.cols;
    float max_distance = -1;

    // Computes the distance between forward and backwards flow
    #pragma omp parallel for
    for(int y = 0; y < h ; ++y)
    {
        for(int x = 0; x < w ; ++x)
        {
            // If there is forward flow for the position F(x,y)
            Vec2f foward = forward_flow.at<Vec2f>(y, x);
            if(foward[0] != kFLOW_UNKNOWN[0] && foward[1] != kFLOW_UNKNOWN[1])
            {
                Vec2i next_position = getAbsoluteFlow(x, y, foward, h, w);
                if(next_position != kPOSITION_INVALID)
                {
                    // If there is backward flow for the refered position B(x + F(x,y).x,y + F(x,y).y)
                    Vec2f backward = backward_flow.at<Vec2f>(next_position[0], next_position[1]);
                    if(backward[0] != kFLOW_UNKNOWN[0] && backward[1] != kFLOW_UNKNOWN[1])
                    {
                        // computes the distance
                        float distance = (float)norm(foward + backward);

                        // Updates the max distance, if required
                        if(distance > max_distance)
                        {
                            #pragma omp critical
                            {
                                if(distance > max_distance)
                                    max_distance = distance;
                            }
                        }

                        // Updates the distance map
                        distances.at<float>(y, x) = distance;
                    }
                }
            }
        }
    }

    // If there is a difference between F and B
    if(max_distance > 0)
    {
        // Computes the normalized confidence map
        #pragma omp parallel for
        for(int y = 0; y < h ; ++y)
        {
            for(int x = 0; x < w ; ++x)
            {
                //printf("\n y is %d \n",y);
                //printf("\n x is %d \n",x);
                //printf("this distance is %f!\n", distances.at<float>(y, x));
                if(distances.at<float>(y, x) < 0)
                {
                    // Unknown flow, C = 0
                    distances.at<float>(y, x) = 0;
                    //printf("0 max distance!\n\n");
                }
                else
                {
                    // C = 1 - normalized distance
                    //printf("max distance is %f\n", max_distance);
                    distances.at<float>(y, x) = (max_distance - distances.at<float>(y, x)) / max_distance;
                    //printf("result distance is %f!\n\n", distances.at<float>(y, x));
                }
                if (distances.at<float>(y, x) > 1 || distances.at<float>(y, x) < 0) printf("wrong!");
            }
        }
        //printf("max distance is %d\n", max_distance);
        return distances;
    }
    else
    {
        // Forward and backwards flow are the same, C = 1
        return Mat1f::ones(forward_flow.rows, forward_flow.cols);
    }
}



template <class TSrc>
Mat1f computeSpatialPermeability(Mat_<TSrc> src, float delta_XY, float alpha_XY)
{
    //printf("bp2.0");
    Mat_<TSrc> I = src;
    int h = I.rows;
    int w = I.cols;
    int num_channels = I.channels();

    // horizontal & vertical difference

    Mat_<TSrc> I_shifted = Mat_<TSrc>::zeros(h,w);
    Mat warp_mat = (Mat_<double>(2,3) <<1,0,-1, 0,1,0);
    warpAffine(I, I_shifted, warp_mat, I_shifted.size()); // could also use rect() to translate image

    // Equation 3.2 in Michel's Paper
    Mat_<TSrc> diff_perm = Mat_<TSrc>::zeros(h,w);
    diff_perm = I - I_shifted; // Ip - Ip' with 3 channels
    std::vector<Mat1f> diff_channels(num_channels);
    split(diff_perm, diff_channels);
    Mat1f dJdx = Mat1f::zeros(h, w);
    //printf("bp2.1");
    for (int c = 0; c < num_channels; c++) // ||Ip - Ip'|| via spliting 3 channels and do pow() and then sum them up and then sqrt()
    {
        Mat1f temp = Mat1f::zeros(h,w);
        temp = diff_channels[c];
        pow(temp, 2, temp);
        dJdx = dJdx + temp;
    }
    //printf("bp2.3");
    sqrt(dJdx, dJdx);
    //printf("bp2.4");
    Mat1f result = Mat1f::zeros(h,w); // finish the rest of the equation and return permeability map stored in "result"
    result = abs(dJdx / (sqrt(3) * delta_XY) );
    pow(result, alpha_XY, result);
    pow(1 + result, -1, result);
    //printf("bp2.5");
    return result;
}

template <class TSrc, class TValue>
//Mat_<TValue> filterXY(Mat_<TSrc> src, Mat_<TValue> J, float iterations_para = 5, int lambda_XY_para = 0, float delta_XY_para = 0.017, float alpha_XY_para = 2)
Mat_<TValue> filterXY(Mat_<TSrc> src, Mat_<TValue> J, cpm_pf_params_t &cpm_pf_params)
{
    //printf("bp1");
    // Input image
    Mat_<TSrc> I = src;
    int h = I.rows;
    int w = I.cols;

    // Joint image (optional).
/*
    Mat_<TRef> A = I;
    if (!joint_image.empty())
    {
        // Input and joint images must have equal width and height.
        assert(src.size() == joint_image.size());
        A = joint_image;
    }
*/
    //printf("bp2");
    // intilizations (move outside later)
//change here
    float iterations = cpm_pf_params.iterations_input_int;
    int lambda_XY = cpm_pf_params.lambda_XY_input_float;
    float delta_XY = cpm_pf_params.delta_XY_input_float;
    float alpha_XY = cpm_pf_params.alpha_XY_input_float;
    //float iterations = 5;
    //int lambda_XY = 0;
    //float delta_XY = 0.017;
    //float alpha_XY = 2;

    // spatial filtering
    int num_chs = J.channels();
    Mat_<TValue> J_XY = J;
    Mat_<TValue> Mat_Ones = Mat_<TValue>::ones(1,1);

    // set outliers to 0, which is 1*10^10 in .flo file
    /*
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            if (num_chs > 1) {
                for (int c = 0; c < num_chs; c++) {
                    if (J_XY(y, x)[c] == kMOVEMENT_UNKNOWN) J_XY(y, x)[c] = 0;
                }
            }
            else {
                //if (J_XY(y, x)[0] == kMOVEMENT_UNKNOWN) J_XY(y, x) = 0;
            //}
        }
    }
    */

    //compute spatial permeability
    //compute horizontal filtered image
    Mat1f perm_horizontal;
    Mat1f perm_vertical;
    perm_horizontal = computeSpatialPermeability<TSrc>(I, delta_XY, alpha_XY);
    //perm_horizontal = perm_horizontal * 255;
    //compute vertial filtered image
    Mat_<TSrc> I_t = I.t();
    perm_vertical = computeSpatialPermeability<TSrc>(I_t, delta_XY, alpha_XY);
    perm_vertical = perm_vertical.t();
    //perm_vertical = perm_vertical * 255;
/*
    for (int x=0, y=0; y < 1 && x < 500; x++) {
        printf("\n y is %d \n",y);
        printf("\n x is %d \n",x);
        printf("J_XYx is %f\n",J_XY(y, x)[0]);
        printf("perm_hori is %f\n",perm_horizontal(y, x));
    }
*/
/*
    namedWindow( "perm_hor_window", WINDOW_AUTOSIZE );
    imshow("perm_hor_window", perm_horizontal);
    namedWindow( "perm_ver_window", WINDOW_AUTOSIZE );
    imshow("perm_ver_window", perm_vertical);
    waitKey(0);
*/

    //Mat1b perm_horizontal_1b;
    //perm_horizontal.convertTo(perm_horizontal_1b,CV_8U,255.);
    //imwrite("perm_horizontal.png", perm_horizontal_1b);



    for (int i = 0; i < iterations; ++i) {
    //for (int i = 0; i < 5; ++i) {
        //printf("bp3\n");
        // spatial filtering
        // Equation 3.7~3.9 in Michel's thesis (lambda is 0 for flow map)

        // horizontal
        //printf("testnum is %f \n",perm_horizontal(0,i));
        Mat_<TValue> J_XY_upper_h = Mat_<TValue>::zeros(h,w); //upper means upper of fractional number
        Mat_<TValue> J_XY_lower_h = Mat_<TValue>::zeros(h,w);
        for (int y = 0; y < h; y++) {
            Mat_<TValue> lp = Mat_<TValue>::zeros(1,w);
            Mat_<TValue> lp_normal = Mat_<TValue>::zeros(1,w);
            Mat_<TValue> rp = Mat_<TValue>::zeros(1,w);
            Mat_<TValue> rp_normal = Mat_<TValue>::zeros(1,w);
/*
            TValue* J_XY_row = J_XY.template ptr<TValue>(y);
            TValue* lp_row = lp.template ptr<TValue>(0);
            TValue* lp_normal_row = lp_normal.template ptr<TValue>(0);
            TValue* rp_row = rp.template ptr<TValue>(0);
            TValue* rp_normal_row = rp_normal.template ptr<TValue>(0);
            float* perm_horizontal_row = perm_horizontal.ptr<float>(y);
            TValue* J_XY_upper_row = J_XY_upper_h.template ptr<TValue>(y);
            TValue* J_XY_lower_row = J_XY_lower_h.template ptr<TValue>(y);
*/
            //printf("bp4\n");
            // left pass
            for (int x = 1; x <= w-1; x++) {

                for (int c = 0; c < num_chs; c++) {
                    lp(0, x)[c] = perm_horizontal(y, x - 1) * (lp(0, x - 1)[c] + J_XY(y, x - 1)[c]);
                    lp_normal(0, x)[c] = perm_horizontal(y, x - 1) * (lp_normal(0, x - 1)[c] + 1.0);
                    //lp(0, x) = perm_horizontal(y, x - 1) * (lp(0, x - 1) + J_XY(y, x - 1));
                    //lp_normal(0, x) = perm_horizontal(y, x - 1) * (lp_normal(0, x - 1) + 1.0);

                    //float* lp_data = lp.data;
                    //float* perm_horizontal_data = perm_horizontal.data;
                    //float* J_XY_data = J_XY.data;

/*
                    if (y < 10) {
                        if (x < 10) {
                            printf("y is %d\n", y);
                            printf("x is %d\n", x);
                            printf("c is %d\n", c);
                            printf("perm_horizontal(y, x - 1) is %f\n", perm_horizontal(y, x - 1));
                            printf("lp(0, x - 1)[c] is %f\n", lp(0, x - 1)[c]);
                            printf("J_XY(y, x - 1)[c] is %f\n", J_XY(y, x - 1)[c]);
                            printf("lp(0, x)[c] is %f\n", lp(0, x)[c]);
                        }
                    }
*/
                }

/*
                lp_row[x] = perm_horizontal_row[x - 1] * (lp_row[x - 1] + J_XY_row[x - 1]);
                lp_normal_row[x] = perm_horizontal_row[x - 1] * (lp_normal_row[x - 1] + Mat_Ones.template at<TValue>(0, 0));

                if (y < 10) {
                    if (x < 10) {
                        printf("y is %d\n", y);
                        printf("x is %d\n", x);
                        //printf("c is %d\n", c);
                        printf("perm_horizontal_row[x-1] is %f\n", perm_horizontal_row[x-1]);
                        printf("lp_row[x-1] is %f\n", lp_row[x-1]);
                        printf("J_XY_row[x-1] is %f\n", J_XY_row[x-1]);
                        printf("lp_row[x] is %f\n", lp_row[x]);
                    }
                }
*/
            }


            //printf("bp5\n");
            // right pass & combining
            for (int x = w-2; x >= 0; x--) {

                for (int c = 0; c < num_chs; c++) {
                    rp(0, x)[c] = perm_horizontal(y, x) * (rp(0, x + 1)[c] + J_XY(y, x + 1)[c]);
                    rp_normal(0, x)[c] = perm_horizontal(y, x) * (rp_normal(0, x + 1)[c] + 1.0);
                    //rp(0, x) = perm_horizontal(y, x) * (rp(0, x + 1) + J_XY(y, x + 1));
                    //rp_normal(0, x) = perm_horizontal(y, x) * (rp_normal(0, x + 1) + 1.0);
                    //combination in right pass loop on-the-fly & deleted source image I
                    if (x == w-2) {
                        //divide(lp(y, w-1) + (1 - lambda_XY) * J_XY(y, w-1) + rp(y, w-1), lp_normal(y, w-1) + Vec2f(1.0, 1.0) + rp_normal(y, w-1), reuslt_J_XY(y, w-1));
                        J_XY(y, x+1)[c] = (lp(0, x+1)[c] + (1 - lambda_XY) * J_XY(y, x+1)[c] + rp(0, x+1)[c]) / (lp_normal(0, x+1)[c] + 1.0 + rp_normal(0, x+1)[c]);
                        //J_XY(y, x+1) = (lp(0, x+1) + (1 - lambda_XY) * J_XY(y, x+1) + rp(0, x+1)) / (lp_normal(0, x+1) + 1.0 + rp_normal(0, x+1));
                    }

                    //divide(lp(y, x) + (1 - lambda_XY) * J_XY(y, x) + rp(y, x), lp_normal(y, x) + Vec2f(1.0, 1.0) + rp_normal(y, x), reuslt_J_XY(y, x));
                    J_XY(y, x)[c] = (lp(0, x)[c] + (1 - lambda_XY) * J_XY(y, x)[c] + rp(0, x)[c]) / (lp_normal(0, x)[c] + 1.0 + rp_normal(0, x)[c]);
                    //J_XY(y, x) = (lp(0, x) + (1 - lambda_XY) * J_XY(y, x) + rp(0, x)) / (lp_normal(0, x) + 1.0 + rp_normal(0, x));

/*
                    if (y == 2 && c == 0) {
                        printf("\n y is %d \n",y);
                        printf("\n x is %d \n",x);
                        printf("\n c is %d \n",c);

                        printf("\nJ_XYx-1 is %f\n",J_XY(y, x-1)[c]);
                        printf("lpx-1 is %f\n",lp(0, x-1)[c]);
                        printf("lpx is %f\n",lp(0, x)[c] );
                        printf("lp_normalx-1 is %f\n",lp_normal(0, x-1)[c] );
                        printf("lp_normalx is %f\n",lp_normal(0, x)[c] );

                        printf("\nJ_XYx+1 is %f\n",J_XY(y, x+1)[c]);
                        printf("rpx+1 is %f\n",rp(0, x+1)[c]);
                        printf("rpx is %f\n",rp(0, x)[c] );
                        printf("rp_normalx+1 is %f\n",rp_normal(0, x+1)[c] );
                        printf("rp_normalx is %f\n",rp_normal(0, x)[c] );
                        printf("perm_hori is %f\n",perm_horizontal(y, x));

                        printf("\nlpx is %f\n",lp(0, x)[c] );
                        printf("J_XYx is %f\n",J_XY(y, x)[c]);
                        printf("rpx is %f\n",rp(0, x)[c] );
                        printf("lp_normalx is %f\n",lp_normal(0, x)[c] );
                        printf("rp_normalx is %f\n",rp_normal(0, x)[c] );
                        printf("Fenzi is %f\n", (lp(y, x)[c] + (1 - lambda_XY) * J_XY(y, x)[c] + rp(y, x)[c]));
                        printf("result_J_XYx is %f\n",J_XY(y, x)[c]);
                    }
*/
                }



/*
                rp_row[x] = perm_horizontal_row[x] * (rp_row[x + 1] + J_XY_row[x + 1]);
                rp_normal_row[x] = perm_horizontal_row[x] * (rp_normal_row[x + 1] + Mat_Ones.template at<TValue>(0, 0));

                //combination in right pass loop on-the-fly & deleted source image I
                if (x == w-2) {
                    J_XY_upper_row[x+1] = lp_row[x + 1] + (1 - lambda_XY) * J_XY_row[x + 1] + rp_row[x + 1];
                    J_XY_lower_row[x+1] = lp_normal_row[x + 1] + Mat_Ones.template at<TValue>(0, 0) + rp_normal_row[x + 1];
                }
               J_XY_upper_row[x] = lp_row[x] + (1 - lambda_XY) * J_XY_row[x] + rp_row[x];
               J_XY_lower_row[x] = lp_normal_row[x] + Mat_Ones.template at<TValue>(0, 0) + rp_normal_row[x];
*/
            }
            //printf("bp6\n");
        }
/*
        vector<Mat> upper_channels_h(num_chs);
        vector<Mat> lower_channels_h(num_chs);
        split(J_XY_upper_h, upper_channels_h);
        split(J_XY_lower_h, lower_channels_h);
        for (int c = 0; c < num_chs; c++) {
            divide(upper_channels_h[c], lower_channels_h[c], upper_channels_h[c]);
        }
        Mat_<TValue> J_XY_merged_h;
        merge(upper_channels_h, J_XY_merged_h);
        J_XY = J_XY_merged_h;
*/
        //namedWindow( "img_window", WINDOW_AUTOSIZE );
        //imshow( "img_window", J_XY );
        //waitKey(0);
        //imwrite("hori_result_iter5.jpg", J_XY * 255.);




        //vertical
        Mat_<TValue> J_XY_upper_v = Mat_<TValue>::zeros(h,w);
        Mat_<TValue> J_XY_lower_v = Mat_<TValue>::zeros(h,w);
        for (int x = 0; x < w; x++) {
            Mat_<TValue> dp = Mat_<TValue>::zeros(h,1);
            Mat_<TValue> dp_normal = Mat_<TValue>::zeros(h,1);
            Mat_<TValue> up = Mat_<TValue>::zeros(h,1);
            Mat_<TValue> up_normal = Mat_<TValue>::zeros(h,1);

            // (left pass) down pass
            for (int y = 1; y <= h-1; y++) {
                for (int c = 0; c < num_chs; c++) {
                    dp(y, 0)[c] = perm_vertical(y - 1, x) * (dp(y - 1, 0)[c] + J_XY(y - 1, x)[c]);
                    dp_normal(y, 0)[c] = perm_vertical(y - 1, x) * (dp_normal(y - 1, 0)[c] + 1.0);
                    //dp(y, 0) = perm_vertical(y - 1, x) * (dp(y - 1, 0) + J_XY(y - 1, x));
                    //dp_normal(y, 0) = perm_vertical(y - 1, x) * (dp_normal(y - 1, 0) + 1.0);

/*
                    if (x == 2 && c == 0) {
                            printf("\n y is %d \n",y);
                            printf("\n x is %d \n",x);
                            printf("\n c is %d \n",c);

                            printf("\nJ_XYy-1 is %f\n",J_XY(y-1, x)[c]);
                            printf("dpy-1 is %f\n",dp(y-1, 0)[c]);
                            printf("dpy is %f\n",dp(y, 0)[c] );
                            printf("dp_normaly-1 is %f\n",dp_normal(y-1, 0)[c] );
                            printf("dp_normaly is %f\n",dp_normal(y, 0)[c] );

                            printf("perm_verty-1 is %f\n",perm_vertical(y-1, x));
                    }
*/

                }

/*
                TValue& dp_xy = dp.template at<TValue>(y, 0);
                TValue& dp_normal_xy = dp_normal.template at<TValue>(y, 0);

                dp_xy = perm_vertical.at<float>(y - 1, x) * (dp.template at<TValue>(y - 1, 0) + J_XY.template at<TValue>(y - 1, x));
                dp_normal_xy = perm_vertical.at<float>(y - 1, x) * (dp_normal.template at<TValue>(y - 1, 0) + Mat_Ones.template at<TValue>(0, 0));
*/
            }

            // (right pass) up pass & combining
            for (int y = h-2; y >= 0; y--) {

                for (int c = 0; c < num_chs; c++) {
                    up(y, 0)[c] = perm_vertical(y, x) * (up(y + 1, 0)[c] + J_XY(y + 1, x)[c]);
                    up_normal(y, 0)[c] = perm_vertical(y, x) * (up_normal(y + 1, 0)[c] + 1.0);
                    //up(y, 0) = perm_vertical(y, x) * (up(y + 1, 0) + J_XY(y + 1, x));
                    //up_normal(y, 0) = perm_vertical(y, x) * (up_normal(y + 1, 0) + 1.0);

                    if(y == h-2) {
                        J_XY(y+1, x)[c] = (dp(y+1, 0)[c] + (1 - lambda_XY) * J_XY(y+1, x)[c] + up(y+1, 0)[c]) / (dp_normal(y+1, 0)[c] + 1.0 + up_normal(y+1, 0)[c]);
                        //J_XY(y+1, x) = (dp(y+1, 0) + (1 - lambda_XY) * J_XY(y+1, x) + up(y+1, 0)) / (dp_normal(y+1, 0) + 1.0 + up_normal(y+1, 0));
                    }
                    J_XY(y, x)[c] = (dp(y, 0)[c] + (1 - lambda_XY) * J_XY(y, x)[c] + up(y, 0)[c]) / (dp_normal(y, 0)[c] + 1.0 + up_normal(y, 0)[c]);
                    //J_XY(y, x) = (dp(y, 0) + (1 - lambda_XY) * J_XY(y, x) + up(y, 0)) / (dp_normal(y, 0) + 1.0 + up_normal(y, 0));
/*
                    if (x == 2 && c == 0) {
                            printf("\n y is %d \n",y);
                            printf("\n x is %d \n",x);
                            printf("\n c is %d \n",c);

                            //printf("\nJ_XYy-1 is %f\n",J_XY(y-1, x)[c]);
                            //printf("dpy-1 is %f\n",dp(y-1, 0)[c]);
                            //printf("dpy is %f\n",dp(y, 0)[c] );
                            //printf("dp_normaly-1 is %f\n",dp_normal(y-1, 0)[c] );
                            //printf("dp_normaly is %f\n",dp_normal(y, 0)[c] );

                            printf("\nJ_XYy+1 is %f\n",J_XY(y+1, x)[c]);
                            printf("upy+1 is %f\n",up(y+1, 0)[c]);
                            printf("upy is %f\n",up(y, 0)[c] );
                            printf("up_normaly+1 is %f\n",up_normal(y+1, 0)[c] );
                            printf("up_normaly is %f\n",up_normal(y, 0)[c] );
                            printf("perm_vert is %f\n",perm_vertical(y, x));

                            printf("\ndpy is %f\n",dp(y, 0)[c] );
                            printf("J_XYy is %f\n",J_XY(y, x)[c]);
                            printf("upy is %f\n",up(y, 0)[c] );
                            printf("dp_normaly is %f\n",dp_normal(y, 0)[c] );
                            printf("up_normaly is %f\n",up_normal(y, 0)[c] );
                            printf("Fenzi is %f\n", (dp(y, 0)[c] + (1 - lambda_XY) * J_XY(y, x)[c] + up(y, 0)[c]));
                            printf("result_J_XYx is %f\n",J_XY(y, x)[c]);
                    }
*/
                }

/*
                TValue& up_xy = up.template at<TValue>(y, 0);
                TValue& up_normal_xy = up_normal.template at<TValue>(y, 0);

                up_xy = perm_vertical.at<float>(y, x) * (up.template at<TValue>(y + 1, 0) + J_XY.template at<TValue>(y + 1, x));
                up_normal_xy = perm_vertical.at<float>(y, x) * (up_normal.template at<TValue>(y + 1, 0) + Mat_Ones.template at<TValue>(0, 0));


                if(y == h-2) {
                    TValue& J_XY_upper_xy_1 = J_XY_upper_v.template at<TValue>(y + 1, x);
                    TValue& J_XY_lower_xy_1 = J_XY_lower_v.template at<TValue>(y + 1, x);
                    J_XY_upper_xy_1 = dp.template at<TValue>(y + 1, 0) + (1 - lambda_XY) * J_XY.template at<TValue>(y + 1, x) + up.template at<TValue>(y + 1, 0);
                    J_XY_lower_xy_1 = dp_normal.template at<TValue>(y + 1, 0) + Mat_Ones.template at<TValue>(0, 0) + dp_normal.template at<TValue>(y + 1, 0);
                }
                TValue& J_XY_upper_xy = J_XY_upper_v.template at<TValue>(y, x);
                TValue& J_XY_lower_xy = J_XY_lower_v.template at<TValue>(y, x);
                J_XY_upper_xy = dp.template at<TValue>(y, 0) + (1 - lambda_XY) * J_XY.template at<TValue>(y, x) + up.template at<TValue>(y, 0);
                J_XY_lower_xy = dp_normal.template at<TValue>(y, 0) + Mat_Ones.template at<TValue>(0, 0) + up_normal.template at<TValue>(y, 0);
 */
            }
        }
/*
        vector<Mat> upper_channels_v(num_chs);
        vector<Mat> lower_channels_v(num_chs);
        split(J_XY_upper_v, upper_channels_v);
        split(J_XY_lower_v, lower_channels_v);
        for (int c = 0; c < num_chs; c++) {
            divide(upper_channels_v[c], lower_channels_v[c], upper_channels_v[c]);
        }
        Mat_<TValue> J_XY_merged_v;
        merge(upper_channels_v, J_XY_merged_v);
        J_XY = J_XY_merged_v;
*/

//vertical end
        //namedWindow( "img_window2", WINDOW_AUTOSIZE );
        //imshow( "img_window2", J_XY );
        //waitKey(0);
        //imwrite("vert_result_iter5.jpg", J_XY * 255.);

    }
    //WriteFlowFile(J_XY, "flow2_vert_result_iter5.flo");

    //std::vector<Mat> J_XY_components(num_components);
    //J_XY_components.push_back((Mat)J_x);
    //J_XY_components.push_back((Mat)J_y);
    //Mat J_XY;
    //merge(J_XY_components, num_components, J_XY);

    return J_XY;
}

template <class TSrc>
Mat1f computeTemporalPermeability(Mat_<TSrc> I, Mat_<TSrc> I_prev, Mat2f flow_XY, Mat2f flow_prev_XYT, float delta_photo, float delta_grad, float alpha_photo, float alpha_grad)
{
    int h = I.rows;
    int w = I.cols;
    int num_channels = I.channels();
    int num_channels_flow = flow_XY.channels();

    Mat1f perm_temporal = Mat1f::zeros(h,w);
    Mat1f perm_gradient = Mat1f::zeros(h,w);
    Mat1f perm_photo = Mat1f::zeros(h,w);

    printf("bpt1.1\n");


    //printf("h is %d\n",h);
    //printf("w is %d\n",w);
    //int hh = flow_prev_maps[0].rows;
    //int ww = flow_prev_maps[0].cols;
    //printf("hh is %d\n",hh);
    //printf("ww is %d\n",ww);


    printf("bpt1.2\n");
    Mat_<TSrc> I_prev_warped = Mat_<TSrc>::zeros(h,w);

    Mat2f prev_map = Mat2f::zeros(h,w);
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++){
            prev_map(y,x)[0] = x - flow_prev_XYT(y,x)[0];
            prev_map(y,x)[1] = y - flow_prev_XYT(y,x)[1];
        }
    }
    std::vector<Mat1f> prev_maps(num_channels_flow);
    split(prev_map, prev_maps);
    remap(I_prev, I_prev_warped, prev_maps[0], prev_maps[1], cv::INTER_CUBIC);
    /*
    imwrite("I_prev_warped_negative.jpg", I_prev_warped * 255.);
    namedWindow( "img_window", WINDOW_AUTOSIZE );
    imshow( "img_window", I_prev );
    namedWindow( "remap_window", WINDOW_AUTOSIZE );
    imshow( "remap_window", I_prev_warped );
    waitKey(0);
    */
    // Equation 11
    printf("bpt1.3\n");
    Mat_<TSrc> diff_I = I - I_prev_warped;
    std::vector<Mat1f> diff_I_channels(num_channels);
    split(diff_I, diff_I_channels);
    Mat1f sum_diff_I = Mat1f::zeros(h, w);
    for (int c = 0; c < num_channels; c++)
    {
        Mat1f temp = Mat1f::zeros(h,w);
        temp = diff_I_channels[c];
        pow(temp, 2, temp);
        sum_diff_I = sum_diff_I + temp;
    }
    sqrt(sum_diff_I, sum_diff_I);
    pow(abs(sum_diff_I / (sqrt(3) * delta_photo)), alpha_photo, perm_photo);
    pow(1 + perm_photo, -1, perm_photo);

/*
    namedWindow( "perm_photo_window", WINDOW_AUTOSIZE );
    imshow( "perm_photo_window", perm_photo );
    waitKey(0);
    //imwrite("perm_photo.jpg", perm_photo * 255.);
*/


    Mat2f flow_prev_XYT_warped  = Mat2f::zeros(h,w);;
    remap(flow_prev_XYT, flow_prev_XYT_warped, prev_maps[0], prev_maps[1], cv::INTER_CUBIC);
    // Equation 12
    printf("bpt1.5\n");
    Mat2f diff_flow = flow_XY - flow_prev_XYT_warped;
    std::vector<Mat1f> diff_flow_channels(num_channels_flow);
    split(diff_flow, diff_flow_channels);
    Mat1f sum_diff_flow = Mat1f::zeros(h, w);
    for (int c = 0; c < num_channels_flow; c++)
    {
        Mat1f temp = Mat1f::zeros(h,w);
        temp = diff_flow_channels[c];
        pow(temp, 2, temp);
        sum_diff_flow = sum_diff_flow + temp;
    }
    sqrt(sum_diff_flow, sum_diff_flow);
    pow(abs(sum_diff_flow / (sqrt(2) * delta_grad)), alpha_grad, perm_gradient);
    pow(1 + perm_gradient, -1, perm_gradient);
/*
    namedWindow( "perm_gradient_window", WINDOW_AUTOSIZE );
    imshow( "perm_gradient_window", perm_gradient );
    waitKey(0);
    //imwrite("perm_gradient.jpg", perm_gradient * 255.);
*/


    printf("bpt1.6\n");

    Mat perm_temporalMat = perm_photo.mul(perm_gradient);
    printf("bpt1.7\n");
    perm_temporalMat.convertTo(perm_temporal, CV_32FC1);
    printf("bpt1.8\n");
    //namedWindow( "perm_temporal_window", WINDOW_AUTOSIZE );
    //imshow( "perm_temporal_window", perm_temporal );
    //waitKey(0);
    return perm_temporal;
}

template <class TSrc, class TValue>
//Mat_<TValue> filterT(Mat_<TSrc> src, Mat_<TSrc> src_prev, Mat_<TValue> J_XY, Mat_<TValue> J_prev_XY, Mat2f flow_XY, Mat2f flow_prev_XYT, Mat_<TValue> l_t_prev, Mat_<TValue> l_t_normal_prev)
vector<Mat_<TValue> > filterT(Mat_<TSrc> src, Mat_<TSrc> src_prev, Mat_<TValue> J_XY, Mat_<TValue> J_prev_XY, Mat2f flow_XY, Mat2f flow_prev_XYT, Mat_<TValue> l_t_prev, Mat_<TValue> l_t_normal_prev)
{
    //store result variable
    vector<Mat_<TValue> > result;

    // Input image
    Mat_<TSrc> I = src;
    Mat_<TSrc> I_prev = src_prev;
    int h = I.rows;
    int w = I.cols;
    int num_channels = I.channels();
    int num_channels_flow = J_XY.channels();
/*
    // Joint image (optional).
    Mat_<TRef> A = I;
    if (!joint_image.empty())
    {
        // Input and joint images must have equal width and height.
        assert(src.size() == joint_image.size());
        A = joint_image;
    }
*/


    Mat_<TValue> J_XYT = J_XY;

    // Initialization parameters
    float lambda_T = 0;
    float delta_photo = 0.3;
    float delta_grad = 1.0;
    float alpha_photo = 2.0;
    float alpha_grad = 2.0;

    // temporal filtering
    int iterations = 1;


    //compute temporal permeability in this iteration
    Mat1f perm_temporal;
    perm_temporal = computeTemporalPermeability<TSrc>(I, I_prev, flow_XY, flow_prev_XYT, delta_photo, delta_grad, alpha_photo, alpha_grad);


    for (int i = 0; i < iterations; ++i)
    //for (int i = 0; i < 1; ++i)
    {
        printf("bpt1\n");
        printf("bpt2\n");

        //split flow
        //change flow file format to remap function required format
        Mat2f prev_map = Mat2f::zeros(h,w);
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++){
                prev_map(y,x)[0] = x - flow_prev_XYT(y,x)[0];
                prev_map(y,x)[1] = y - flow_prev_XYT(y,x)[1];
            }
        }
        std::vector<Mat1f> flow_XYT_prev_maps(num_channels_flow);
        split(prev_map, flow_XYT_prev_maps);

        // temporal filtering
        // Equation 3.7~3.9 in Michel's thesis (lambda is 0 for flow map)

        printf("bpt3\n");
        // horizontal
        Mat_<TValue> l_t = Mat_<TValue>::zeros(h, w);
        Mat_<TValue> l_t_normal = Mat_<TValue>::zeros(h, w);
        //Mat1f r = Mat::zeros(h,w);
        //Mat1f r_normal = Mat::zeros(h,w);

        printf("bpt4\n");
        // no need to do pixel-wise operation (via J(y,x)) since all operation is based on same location pixels
        // (left pass) forward pass & combining
        Mat_<TValue> temp_l_t_prev = Mat_<TValue>::zeros(h, w);
        Mat_<TValue> temp_l_t_prev_warped = Mat_<TValue>::zeros(h, w);
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                for (int c = 0; c < num_channels_flow; c++) {
                    temp_l_t_prev(y,x)[c] = l_t_prev(y,x)[c] + J_prev_XY(y,x)[c];
                }
            }
        }
        //temp_l_t_prev = l_t_prev + J_prev_XY;
        printf("bpt5\n");
        remap(temp_l_t_prev, temp_l_t_prev_warped, flow_XYT_prev_maps[0], flow_XYT_prev_maps[1], cv::INTER_CUBIC);
        //WriteFlowFile(temp_l_t_prev, "temp_l_t_prev.flo");
        //WriteFlowFile(temp_l_t_prev_warped, "temp_l_t_prev_warped.flo");
        //std::vector<Mat1f> temp_l_t_prev_chs(num_channels_flow);
        //split(temp_l_t_prev, temp_l_t_prev_chs);
        printf("bpt6\n");
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                for (int c = 0; c < num_channels_flow; c++) {
                    l_t(y,x)[c] = perm_temporal(y,x) * temp_l_t_prev_warped(y,x)[c];
                }
            }
        }
        printf("bpt7\n");

        Mat_<TValue> temp_l_t_normal_prev = Mat_<TValue>::zeros(h, w);
        Mat_<TValue> temp_l_t_normal_prev_warped = Mat_<TValue>::zeros(h, w);
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                for (int c = 0; c < num_channels_flow; c++) {
                    temp_l_t_normal_prev(y,x)[c] = l_t_normal_prev(y,x)[c] + 1.0;
                }
            }
        }
        printf("bpt8\n");
        remap(temp_l_t_normal_prev, temp_l_t_normal_prev_warped, flow_XYT_prev_maps[0], flow_XYT_prev_maps[1], cv::INTER_CUBIC);
        //WriteFlowFile(temp_l_t_normal_prev, "temp_l_t_normal_prev.flo");
        //WriteFlowFile(temp_l_t_normal_prev_warped, "temp_l_t_normal_prev_warped.flo");
        printf("bpt9\n");
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                for (int c = 0; c < num_channels_flow; c++) {
                    l_t_normal(y,x)[c] = perm_temporal(y,x) * temp_l_t_normal_prev_warped(y,x)[c];
                    J_XYT(y,x)[c] = (l_t(y,x)[c] + (1 - lambda_T) * J_XY(y,x)[c]) / (l_t_normal(y,x)[c]+ 1.0);
                }
            }
        }
        printf("bpt10\n");

        // (right pass) backward pass & combining
        //for (int x = w-1; x >= 0; x--) {
        //    r(y,x) = perm_horizontal(y,x) * ( r(y,x+1) + J_XY(y,x+1) );
        //    r_normal(y,x) = perm_horizontal(y,x) * ( r(y,x+1) + 1.0 );

        //    J_XY = (l + (1-lambda_XY) * J_XY + lambda_XY * I + r) / (l_normal + 1.0 +r_normal);
        //}

        //J_XYT = (l_t + (1 - lambda_T) * J_XY) / (l_t_normal + 1.0);


        printf("bpt11\n");
        //l_t_prev = l_t;
        //l_t_normal_prev = l_t_normal;
        result.push_back(l_t);
        result.push_back(l_t_normal);
/*
        for(int y = 0; y < J_XYT.rows; y++) {
            for(int x = 0; x < J_XYT.cols; x++) {
                for(int c = 0; c < J_XYT.channels(); c++) {
                      printf("y is %d\n", y);
                      printf("x is %d\n", x);
                      printf("c is %d\n", c);
                      //printf("l_t is %f\n", l_t(y,x));
                      printf("l_t is %f\n", l_t(y,x)[c]);
                }
            }
        }
*/
    }



    //namedWindow( "img_window", WINDOW_AUTOSIZE );
    //imshow( "img_window", J_XYT );
    //waitKey(0);
    //imwrite("003_XYT.jpg", J_XYT * 255.);
    //WriteFlowFile(J_XYT, "003_XYT.flo");



    //return J_XYT;
    result.push_back(J_XYT);
    return result;
}



