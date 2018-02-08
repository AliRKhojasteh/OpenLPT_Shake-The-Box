

#include <STB.h>
#include "NumDataIO.h"

#define ALL_CAMS 100
#define REDUCED_CAMS 0
#define STBflag true
#define IPRflag false

using namespace std;

//enum STB::TrackType { Inactive = 0, ActiveShort = 1, ActiveLong = 2, Exit = 3, InactiveLong = 4};

// Remove the 'temp' frame after testing

STB::STB(int firstFrame, int lastFrame, string pfieldfile, string iprfile, int ncams, deque<int> camIDs, 
	deque<string> imageNameFiles, double initialRadius, double avgDist, double expShift, double masc, 
	double mrsc, double fpt, double lowerInt, bool iprFlag) : 

	first(firstFrame), last(lastFrame), iprfile(iprfile), _ipr(iprfile, ncams), pfieldfile(pfieldfile), ncams(ncams), camID(camIDs),
	imgNameFiles(imageNameFiles), searchRadiusSTB(initialRadius), avgIPDist(avgDist), largestShift(expShift), maxAbsShiftChange(masc), 
	maxRelShiftChange(mrsc/100), iprFlag(iprFlag), OTFcalib(ncams, _ipr.otfFile), fpt(fpt), intensityLower(lowerInt)
{ 
// SETTING UP THE PARAMETERS
	// camera parameters
	cams = _ipr.camsAll;
	Npixh = cams[0].Get_Npixh();
	Npixw = cams[0].Get_Npixw();

	// creating & initializing original, residual and reprojected image pixels
	for (int n = 0; n < ncams; n++) {
		pixels_orig.push_back(new int*[Npixh]);
		pixels_reproj.push_back(new int*[Npixh]);
		pixels_res.push_back(new int*[Npixh]);

		for (int i = 0; i < Npixh; i++) {
			pixels_orig[n][i] = new int[Npixw] {};
			pixels_reproj[n][i] = new int[Npixw] {};
			pixels_res[n][i] = new int[Npixw] {};
		}
	}

	it_innerloop = _ipr.it_innerloop;
	tiffaddress = _ipr.tiffaddress;
	
	for (int i = 0; i < ncams; i++) {												// loading image filenames at each frame
		ifstream file(tiffaddress + imgNameFiles[i], ios::in);
		if (file.is_open()) {
			deque<string> imgNames;
			int lineNum = 0;
			for (int frame = 1; frame <= last; frame++) {
				string line;
				getline(file, line); //line.erase(line.find_first_of(' '));
				stringstream tiffname;
				tiffname << tiffaddress << line;
				imgNames.push_back(tiffname.str());
			}
			imgSequence.push_back(imgNames);
		}
		else
			cout << "could not open the image sequence file " << imageNameFiles[i] << endl;
	}

	InitialPhase(pfieldfile);														// Initial phase: obtaining tracks for first 4 time steps

	clock_t start0;
	start0 = clock();
	if (!(_ipr.triangulationOnly) && !(_ipr.IPROnly)) {								// if the user chooses to run only triangulation or IPR, stop here

		// Loading tracks from mat files
	/*	TrackType types;
		types = static_cast<TrackType>(0);
		Load_Tracks("InactiveTracks", types);
		types = static_cast<TrackType>(1);
		Load_Tracks("ActiveShortTracks", types);
		types = static_cast<TrackType>(2);
		Load_Tracks("ActiveLongTracks", types);
		types = static_cast<TrackType>(3);
		Load_Tracks("exitTracks", types);
		types = static_cast<TrackType>(4);
		Load_Tracks("InactiveLongTracks", types);
		cout << "\t\tNo. of active Short tracks:	= " << activeShortTracks.size() << endl;
		cout << "\t\tNo. of active Long tracks:	= " << activeLongTracks.size() << endl;
		cout << "\t\tNo. of exited tracks:		" << exitTracks.size() << endl;
		cout << "\t\tNo. of inactive tracks:		= " << inactiveTracks.size() << endl;
		cout << "\t\tNo. of inactive Long tracks:	= " << inactiveLongTracks.size() << endl;*/

		ConvergencePhase();

		double duration = clock() - start0;

		cout << endl << endl << "\tTotal time taken by STB convergence phase: " << duration / 1000 << "s" << endl;


	}
}

// Initial Phase: for the first 'n = 4' frames, using predictive field for tracking
void STB::InitialPhase(string pfieldfile) {
	int endFrame = last+1;
	if (!(_ipr.triangulationOnly) && !(_ipr.IPROnly)) 
		endFrame = (last >= first + 4) ? first + 4 : last+1;

	for (int frame = first; frame < endFrame; ++frame) {											// ipr on first 4 frames
		std::cout << "IPR on frame " << frame << " of " << last << endl;
																
		if (iprFlag || _ipr.triangulationOnly || _ipr.IPROnly) {									// getting 3D positions using ipr (or)
			Frame all(_ipr.FindPos3D(imgSequence,frame));
			iprMatched.push_back(all);
		}
															
		else {																						// getting 3D positions from .mat files
			string pos3D = "pos3Dframe" + to_string(frame);
			iprMatched.push_back(Load_3Dpoints(pos3D));
		}
	}
																
	if (!(_ipr.triangulationOnly) && !(_ipr.IPROnly)) {												// if the user chooses to run only triangulation or IPR, stop here
																									// else,
		for (int frame = first; frame < endFrame - 1; frame++) {									// using predictive field to get the initial tracks
			int currFrame = frame, nextFrame = frame + 1;
			cout << "STB initial phase tracking b/w frames: " << currFrame << " & " << nextFrame << endl;

																									// getting the predictive velocity field b/w the current and next frames
			PredictiveField pField(iprMatched[currFrame - first], iprMatched[nextFrame - first], pfieldfile, currFrame);	// either calulating it or from .mat files

																									// extending all the tracks that are active in current frame
			for (deque<Track>::iterator tr = activeShortTracks.begin(); tr != activeShortTracks.end(); ) {
				bool active;
				Position currVelocity(pField.ParticleInterpolation(tr->Last()));
				MakeLink(nextFrame, currVelocity, searchRadiusSTB, *tr, active);

				if (!active) 																		// if the track couldn't find a link, then move the track from active to inactive tracks
					tr = activeShortTracks.erase(tr);
				
				else
					++tr;
			}

			StartTrack(currFrame, pField);															// starting a track for all particles left untracked in current frame
		}
																									// moving all tracks longer than 3 from activeShortTracks to activeLongTracks
		for (deque<Track>::iterator tr = activeShortTracks.begin(); tr != activeShortTracks.end(); ) {
			if (tr->Length() > 3) {
				activeLongTracks.push_back(*tr);
				tr = activeShortTracks.erase(tr);
			}
			else
				++tr;
		}
																									// adding the untracked particles from frame 4 to activeShortTracks
		for (Frame::const_iterator pID = iprMatched[(endFrame - 1) - first].begin(); pID != iprMatched[(endFrame - 1) - first].end(); ++pID) {
			if (!(pID->IsTracked())) {
				Track temp(*pID, endFrame - 1);
				activeShortTracks.push_back(temp);
			}
		}

		cout << "Done with initial phase" << endl << endl;

		cout << "\t\tNo. of active Short tracks:	" << activeShortTracks.size() << endl;
		cout << "\t\tNo. of active Long tracks:	" << activeLongTracks.size() << endl;
		cout << "\t\tNo. of exited tracks:		" << exitTracks.size() << endl;
		cout << "\t\tNo. of inactive tracks:		" << inactiveTracks.size() << endl;
	}
}

// Convergence phase: after getting tracks of length 4 from initial phase
void STB::ConvergencePhase() {
	int endFrame = last;

	Calibration calib(_ipr.calibfile, ALL_CAMS, _ipr.mindist_2D, _ipr.mindist_3D, ncams);

	for (int currFrame = first + 3; currFrame < endFrame; currFrame++) {							// tracking frame by frame using Wiener / polynomial predictor along with shaking
		// initializing some variables
		int c1 = activeShortTracks.size(), c2 = activeLongTracks.size(), c3 = inactiveTracks.size(), c4 = inactiveLongTracks.size();
		a_as = 0; a_al = 0; a_is = 0; s_as1 = 0; a_as2 = 0; s_as3 = 0; s_al = 0; a_il = 0;
		// time
		clock_t start0, start1, start2, start3, start4;
		start0 = clock();

		int nextFrame = currFrame + 1;
		cout << "\tSTB convergence phase tracking at frame: " << nextFrame << endl;
		cout << "\t\tCheck Points: " << endl;

// LOADING IMAGES 
		deque<string> imgNames;
		for (int i = 0; i < ncams; i++)
			imgNames.push_back(imgSequence[i][nextFrame - 1]);

		Tiff2DFinder t(ncams, _ipr.Get_threshold(), imgNames);
		t.FillPixels(pixels_orig);																	// filled pixels_orig with the px of actual camera images, which will be used for shaking

		
// PREDICTION
		cout << "\t\t\tPrediction: ";
		Frame estPos;																				// a frame that saves all the positions estimated from track prediction
		deque<double> estInt;																		// saving the intensity of estimated positions
		start1 = clock();

		Prediction(nextFrame, estPos, estInt);														// getting the prediction list for all the activLongTrakcs (>4)

		start2 = clock();
		cout << "Done (" << (clock() - start1)/1000 << "s)" << endl << "\t\t\tShaking the predictions: " ;

		/* TESTING */ tempPredictions = estPos;

// SHAKING THE PREDICTIONS
		Shake(estPos, estInt);																		// correcting the predicted positions by shaking, removing wrong / ambiguous predictions and updating the residual images

// TESTING PREDICTIONS
		/*_ipr.MatfilePositions(estPos.Get_PosDeque(), tiffaddress + "shakedframe" + to_string(nextFrame));
		_ipr.MatfilePositions(temp.Get_PosDeque(), tiffaddress + "predictionsframe" + to_string(nextFrame));*/
		//if (nextFrame % 100 == 97 || nextFrame % 100 == 98 || nextFrame % 100 == 99 || nextFrame % 100 == 0)
		//	_ipr.MatfileImage(pixels_orig, tiffaddress + "resImg" + to_string(nextFrame));
// END TESTING

		for (int i = 0; i < estPos.NumParticles(); i++) 											// adding the corrected particle position in nextFrame to its respective track 
			activeLongTracks[i].AddNext(estPos[i], nextFrame);
		
		start3 = clock();
		cout << "Done (" << (clock() - start2)/1000 << "s)" << endl << "\t\t\tShort tracks from residuals: ";


// IPR ON RESIDUALS		
		Frame candidates = IPRonResidual(calib, t, pixels_orig, pixels_reproj, pixels_res, estPos);	// applying ipr on residual images to obtain particle candidates

// TESTING IPR AND TRACKING FROM RESIDUALS	
		if (nextFrame % 100 == 97 || nextFrame % 100 == 98 || nextFrame % 100 == 99 || nextFrame % 100 == 0)
			_ipr.MatfilePositions(candidates.Get_PosDeque(), tiffaddress + "pos3Dresframe" + to_string(nextFrame));																				
// END TESTING
																									// trying to link each activeShortTrack with a particle candidate
		for (deque<Track>::iterator tr = activeShortTracks.begin(); tr != activeShortTracks.end(); ) 		
			MakeShortLinkResidual(nextFrame, candidates, tr, 5);																									
																									// moving all activeShortTracks longer than 3 particles to activeLongTracks
		for (deque<Track>::iterator tr = activeShortTracks.begin(); tr != activeShortTracks.end(); ) {
			if (tr->Length() > 3) {
				activeLongTracks.push_back(*tr);
				tr = activeShortTracks.erase(tr);
				s_as3++; a_al++;
			}
			else
				++tr;
		}

		start4 = clock();
		cout << "Done (" << (clock() - start3) / 1000 << "s)" << endl << "\t\t\tPruning the tracks: ";

// PRUNING / ARRANGING THE TRACKS
		double thresh = 1.5 * largestShift;													
		for (deque<Track>::iterator tr = activeLongTracks.begin(); tr != activeLongTracks.end(); ) {
			double d1 = pow(Distance(tr->Last(), tr->Penultimate()),0.5), d2 = pow(Distance(tr->Penultimate(), tr->Antepenultimate()),0.5);
			double threshRel = maxRelShiftChange*d2, length = tr->Length();
																									// moving all activeLongTracks with displacement more than LargestExp shift to inactiveTracks
			if (d1 > thresh) {
				inactiveTracks.push_back(*tr);
				tr = activeLongTracks.erase(tr);
				s_al++; a_is++;
			}
																									// moving all activeLongTracks with large change in particle shift to inactive/inactiveLong tracks 
			else if (abs(d1 - d2) > maxAbsShiftChange || abs(d1 - d2) > threshRel) {
				if (length >= 7) {
					inactiveLongTracks.push_back(*tr);
					tr = activeLongTracks.erase(tr);
					s_al++; a_il++;
				}
				else {
					inactiveTracks.push_back(*tr);
					tr = activeLongTracks.erase(tr);
					s_al++; a_is++;
				}
			}

			else
				++tr;
		}																						

		for (Frame::const_iterator pID = candidates.begin(); pID != candidates.end(); ++pID) {		// adding all the untracked candidates to a new short track 	
			Track startTrack(*pID, nextFrame);
			activeShortTracks.push_back(startTrack); a_as++;
		}

		cout << "Done (" << (clock() - start4) / 1000 << "s)" << endl;

		cout << "\t\tNo. of active Short tracks:	" << c1 << " + " << a_as << " - (" << s_as1 << " + " << a_as2 << " + " << s_as3 << ") = " << activeShortTracks.size() << endl;
		cout << "\t\tNo. of active Long tracks:	" << c2 << " + " << a_al << " - " << s_al << " = " << activeLongTracks.size() << endl;
		cout << "\t\tNo. of exited tracks:		 = " << exitTracks.size() << endl;
		cout << "\t\tNo. of inactive tracks:		" << c3 << " + " << a_is << " = " << inactiveTracks.size() << endl;
		cout << "\t\tNo. of inactive Long tracks:	" << c4 << " + " << a_il << " = " << inactiveLongTracks.size() << endl;

		cout << "\t\tTime taken for STB at frame " << nextFrame << ": " << (clock() - start0) / 1000 << "s" << endl;

		//time_t t = time(0);
		if (nextFrame % 5 == 0) {
			cout << "\tSaving the tracks" << endl;
			MatTracksSave(tiffaddress, "", nextFrame);
		}
	}
}

// a function to start a track
void STB::StartTrack(int frame, PredictiveField& pField) {
	for (Frame::const_iterator pID = iprMatched[frame - first].begin(); pID != iprMatched[frame - first].end(); ++pID) {																									
		if (!(pID->IsTracked())) {																// if the particle is not a part of a track
			Track initialTrack;																		// then start a track
			
			iprMatched[frame - first][pID.where()].SetTracked();									// setting the particle as tracked and adding it to a track
			initialTrack.AddNext(*pID, frame);
			//cout << "(X,Y,Z) = (" << pID->X() << "," << pID->Y() << "," << pID->Z() << ")" << " and track is active: " << initialTrack.IsActive() << endl;
			
			Position currVelocity(pField.ParticleInterpolation(*pID));								// interpolating the velocity field to pID
			
			bool activeTrack;
			
			MakeLink(frame + 1, currVelocity, searchRadiusSTB, initialTrack, activeTrack);			// finding a link for pID in next frame
			
			if (activeTrack)
				activeShortTracks.push_back(initialTrack);

			// setting the track as active: if it finds an element in next frame / inactive: if it doesn't 
			//allTracks.back().SetActive(activeTrack);
		}	
	}
}

// a function to make a link using predictive velocity field
void STB::MakeLink(int nextFrame, const Position& currVelocity, double radius, Track& track, bool& activeTrack)
{
		
		Position estimate;																			// we'll need an estimated future position
		
		estimate = track.Last() + currVelocity;														// the estimated point	
		
		int len = track.Length();																	// length of the track
		
		pair<Frame::const_iterator, float> cost;
		
		if (len == 1) {																		// if the track has only one particle
																									// choosing the particle closest to the estimate
			cost = ComputeCost(iprMatched[nextFrame - first], iprMatched[nextFrame - first], radius, estimate, currVelocity, currVelocity, true);
		}
		else if (len >= 2) {																// if it has more than 2 particles		
			Position prevVelocity = track.Last() - track.Penultimate();								// using the velocity b/w previous 2 frames
			/*if (nextFrame - first < iprMatched.size() - 1) 
				cost = ComputeCost(iprMatched[nextFrame - first], iprMatched[nextFrame - first + 1], radius, estimate, prevVelocity, currVelocity, false);
			
			else */
				cost = ComputeCost(iprMatched[nextFrame - first], iprMatched[nextFrame - first], radius, estimate, prevVelocity, currVelocity, true);
		}
						
		if (cost.second == UNLINKED) {														// if no link found in next frame		
			//track.SetActive(false);																// set as inactive track
			activeTrack = false;
		}
		else {																				// if a link is found																									
			iprMatched[nextFrame - first][cost.first.where()].SetTracked();							// setting the particle from nextFrame as tracked		
			track.AddNext(*cost.first, nextFrame);													// adding the particle to current track
			activeTrack = true;																		// setting the track as still active
			//cout << "(X,Y,Z) = (" << cost.first->X() << "," << cost.first->Y() << "," << cost.first->Z() << ")" << " and track is active: " << track.IsActive() << endl;
		}
}

pair<Frame::const_iterator, float> STB::ComputeCost(Frame& fr1, Frame& fr2, double radius,
									const Position& estimate,
									const Position& prevVelocity,
									const Position& currVelocity,
									bool stopflag)
{
																									// find possible continuations in the next frame.

	deque<Frame::const_iterator> matches;															// find possible matches
	deque<float> match_costs;
	float mincost = radius * radius;

	Frame::const_iterator fitend = fr1.end();
	for (Frame::const_iterator fit = fr1.begin(); fit != fitend; ++fit) {
		float mag = Distance(estimate, *fit);
		if (mag > mincost) {
			continue;
		}
		matches.push_back(fit);
		if (stopflag == true) {
			match_costs.push_back(mag);																// don't look into the future any more
			if (mincost > mag) {
				mincost = mag;
			}
		}
		else {			
			Position acceleration = currVelocity - prevVelocity;									// project again!
			Position new_estimate = *fit + currVelocity + 0.5 * acceleration;
			pair<Frame::const_iterator, float> cost = ComputeCost(fr2, fr2, radius, new_estimate, prevVelocity, currVelocity, true);
			match_costs.push_back(cost.second);
			if (mincost > cost.second) {
				mincost = cost.second;
			}
		}
	}

	deque<Frame::const_iterator>::const_iterator m_end = matches.end();
	pair<Frame::const_iterator, float> retval = make_pair(fitend-1, -1);
	deque<float>::const_iterator mc = match_costs.begin();
	for (deque<Frame::const_iterator>::const_iterator m = matches.begin(); m != m_end; ++m, ++mc) {
		if (*mc > mincost) {
			continue;
		}
		retval = make_pair(*m, *mc);
	}

	return retval;
}

void STB::Prediction(int frame, Frame& estPos, deque<double>& estInt) {
	int t = frame;																			// time
	vector<vector<double>> predCoeff(3);													// using polynomial fit / Wiener filter to predict the particle position at nextFrame
	vector<string> direction = { "X", "Y", "Z" };
	vector<double> est(3);
	deque<Track>::const_iterator tr_end = activeLongTracks.end();								// for each track in activeLongTracks (at least 4 particles long) 
	for (deque<Track>::iterator tr = activeLongTracks.begin(); tr != tr_end; ++tr) {
		for (int i = 0; i < 3; i++) {															// getting predictor coefficients for X->0, Y->1 and Z->2
			if (tr->Length() < 6) {																// if length is 4 or 5, use all points to get 2nd degree polynomial
				predCoeff[i] = Polyfit(*tr, direction[i], tr->Length(), 2);
				est[i] = predCoeff[i][0] + predCoeff[i][1] * t + predCoeff[i][2] * pow(t, 2);
			}
			else if (tr->Length() < 11) {														// if length is 6 to 10, use all points to get 3rd degree polynomial
				predCoeff[i] = Polyfit(*tr, direction[i], tr->Length(), 3);
				//cout << "break" << tr->Length() << endl;
				est[i] = predCoeff[i][0] + predCoeff[i][1] * t + predCoeff[i][2] * pow(t, 2) + predCoeff[i][3] * pow(t, 3);
			}
			else {																				// if length is more than 11, use last 10 points to get 3rd degree polynomial
				predCoeff[i] = Polyfit(*tr, direction[i], 10, 3);
				est[i] = predCoeff[i][0] + predCoeff[i][1] * t + predCoeff[i][2] * pow(t, 2) + predCoeff[i][3] * pow(t, 3);
			}
		}
		Position estimate(est[0], est[1], est[2]);												// estimated position at nextFrame
		estInt.push_back(1);																	// setting initial intensity to 1
		estPos.Add(estimate);
	}
}

// convergence phase
// filter for prediction 
// polynomial fit
vector<double> STB::Polyfit(Track tracks, string direction, int datapoints, int polydegree) {

	int size = tracks.Length();

	double* t = new double[datapoints];
	double* x = new double[datapoints];

	for (int i = 0; i < datapoints; i++) {
		t[i] = tracks.GetTime(size - i - 1);	//time

		if (direction == "X" || direction == "x")
			x[i] = tracks[size - i - 1].X();	//x-values or
		else if (direction == "Y" || direction == "y")
			x[i] = tracks[size - i - 1].Y();	//y-values or
		else if (direction == "Z" || direction == "z")
			x[i] = tracks[size - i - 1].Z();	//z-values
	}

	// n = 3 is the degree;
	double* T = new double[2 * polydegree + 1];                        //Array that will store the values of sigma(xi),sigma(xi^2),sigma(xi^3)....sigma(xi^2n)
	for (int i = 0; i < 2 * polydegree + 1; i++) {
		T[i] = 0;
		for (int j = 0; j<datapoints; j++)
			T[i] = T[i] + pow(t[j], i);        //consecutive positions of the array will store N,sigma(xi),sigma(xi^2),sigma(xi^3)....sigma(xi^2n)
	}
									            
	double** B = new double*[polydegree + 1];	//B is the Normal matrix(augmented) that will store the equations, 'a' is for value of the final coefficients
	for (int i = 0; i < polydegree + 1; i++)
		B[i] = new double[polydegree + 2];
	double* a = new double[polydegree + 1];
	for (int i = 0; i < polydegree + 1; i++)
		a[i] = 0;

	for (int i = 0; i < polydegree + 1; i++) 
		for (int j = 0; j < polydegree + 1; j++)
			B[i][j] = T[i + j];            //Build the Normal matrix by storing the corresponding coefficients at the right positions except the last column of the matrix

	double* X = new double[polydegree + 1];                    //Array to store the values of sigma(yi),sigma(xi*yi),sigma(xi^2*yi)...sigma(xi^n*yi)
	for (int i = 0; i < polydegree + 1; i++) {
		X[i] = 0;
		for (int j = 0; j<datapoints; j++)
			X[i] = X[i] + pow(t[j], i)*x[j];        //consecutive positions will store sigma(yi),sigma(xi*yi),sigma(xi^2*yi)...sigma(xi^n*yi)
	}

	for (int i = 0; i < polydegree + 1; i++)
		B[i][polydegree + 1] = X[i];                //load the values of Y as the last column of B(Normal Matrix but augmented)

	polydegree = polydegree + 1;                //n is made n+1 because the Gaussian Elimination part below was for n equations, but here n is the degree of polynomial and for n degree we get n+1 equations
	//cout << "\nThe Normal(Augmented Matrix) is as follows:\n";

	for (int i = 0; i < polydegree; i++)                     //From now Gaussian Elimination starts(can be ignored) to solve the set of linear equations (Pivotisation)
		for (int k = i + 1; k < polydegree; k++) 
			if (B[i][i] < B[k][i]) 
				for (int j = 0; j <= polydegree; j++) {
					double temp = B[i][j];
					B[i][j] = B[k][j];
					B[k][j] = temp;
				}			

	for (int i = 0; i < polydegree - 1; i++)             //loop to perform the gauss elimination
		for (int k = i + 1; k < polydegree; k++)	{
			double s = B[k][i] / B[i][i];
			for (int j = 0; j <= polydegree; j++)
				B[k][j] = B[k][j] - s*B[i][j];    //make the elements below the pivot elements equal to zero or elimnate the variables		
		}
	
	for (int i = polydegree - 1; i >= 0; i--)                //back-substitution
	{                        //x is an array whose values correspond to the values of x,y,z..
		a[i] = B[i][polydegree];                //make the variable to be calculated equal to the rhs of the last equation
		for (int j = 0; j < polydegree; j++) {
			if (j != i)            //then subtract all the lhs values except the coefficient of the variable whose value is being calculated
				a[i] = a[i] - B[i][j] * a[j];
		}
		a[i] = a[i] / B[i][i];            //now finally divide the rhs by the coefficient of the variable to be calculated
	}

	vector<double> coeff;
	for (int i = 0; i < polydegree; i++) 
		coeff.push_back(a[i]);
	
	polydegree = polydegree - 1;
	for (int i = 0; i < polydegree + 1; i++)
		delete[] B[i];

	delete[] T, x, t, X,a, B;

	return coeff;
}

// For shaking the predicted estimates
void STB::Shake(Frame& estimate, deque<double>& intensity) {
	
	if (estimate.NumParticles() > 0) {
		for (int index = 0; index < estimate.NumParticles(); index++) {						// adding 2D image centers and intensity data to the estimates 
			_ipr.FullData(estimate[index], intensity[index], ncams, ALL_CAMS);
		}

		deque<int> ignoreCam = Rem(estimate, intensity, _ipr.mindist_3D);					// removing ambiguous, ghost and particles that disappear on at least 2 cams

		for (int loopInner = 0; loopInner < _ipr.it_innerloop; loopInner++) {
			double del;
			if (loopInner < 1)  del = _ipr.mindist_2D;			// initial shake
			else if (loopInner < 5)  del = _ipr.mindist_2D/10;	// normal shakes
			else  del = _ipr.mindist_2D/100;

			_ipr.ReprojImage(estimate, OTFcalib, pixels_reproj, IPRflag);					// adding the estimated particles to the reprojected image

			for (int n = 0; n < ncams; n++) 												// updating the residual image by removing the estimates
				for (int i = 0; i < Npixh; i++)
					for (int j = 0; j < Npixw; j++)
						pixels_res[n][i][j] = (pixels_orig[n][i][j] - abs(pixels_reproj[n][i][j]));


			int index = 0;																	// correcting the estimated positions and their intensity by shaking
			for (Frame::const_iterator pID = estimate.begin(); pID != estimate.end(); ++pID) {
				Shaking s(ncams, ignoreCam[index], OTFcalib, Npixw, Npixh, _ipr.psize, del, *pID, cams, pixels_res, intensity[index]);
				estimate[index] = s.Get_posnew();
				intensity[index] = s.Get_int();
				index++;
			}
		}

		for (int index = 0; index < estimate.NumParticles(); index++) {						// adding 2D image centers and intensity data after shaking
			_ipr.FullData(estimate[index], intensity[index], ncams, ALL_CAMS);
		}

		Rem(estimate, intensity, _ipr.mindist_3D);											// removing ambiguous particles and particles that did not find a match on the actual images

		_ipr.ReprojImage(estimate, OTFcalib, pixels_reproj, STBflag);						// updating the reprojected image

		for (int n = 0; n < ncams; n++) 													// updating the residual image
			for (int i = 0; i < Npixh; i++)
				for (int j = 0; j < Npixw; j++) {
					int residual = (pixels_orig[n][i][j] - fpt*pixels_reproj[n][i][j]);		// using the multiplying factor (fpt) to remove all traces of tracked particles
					pixels_orig[n][i][j] = (residual < 0) ? 0 : residual;
				}
	}
	
}

// removing ghost,ambiguous and particles leaving measurement domain
deque<int> STB::Rem(Frame& pos3D, deque<double>& int3D, double mindist_3D) {
	double thresh3D = 4 * mindist_3D * mindist_3D;
	double threshShift = pow(largestShift, 2);
	for (int i = 0; i < pos3D.NumParticles(); i++) {									// deleting particles that are very close to each other
		for (int j = i + 1; j < pos3D.NumParticles(); ) {
			if (Distance(pos3D[i], pos3D[j]) < thresh3D) {
				pos3D.Delete(j); int3D.erase(int3D.begin() + j); tempPredictions.Delete(j);
				inactiveTracks.push_back(activeLongTracks[j]);							// shifting the corresponding activeLongTrack to inactiveTracks
				activeLongTracks.erase(activeLongTracks.begin() + j);
				s_al++; a_is++;
			}
			else
				j++;
		}
	}

	int ghost = 0;																		
	double avgIntTemp = 0, avgInt = 0, count = 0;										// deleting based on intensity
	for (int i = 0; i < int3D.size(); i++) 
		if (int3D[i] > 0) 
			avgIntTemp = avgIntTemp + int3D[i];
		
	avgInt = avgIntTemp;
	avgIntTemp = avgIntTemp / int3D.size();

	for (int i = 0; i < int3D.size(); i++)												// removing the outliers (very bright particles and very dull particles for mean)
		if (int3D[i] > 30*avgIntTemp) {
			avgInt = avgInt - int3D[i];
			count++;
		}
	avgInt = avgInt / (int3D.size()-count);

	for (int index = int3D.size() - 1; index >= 0; index--) {							// removing a particle if its intensity falls below a % of the mean intensity
		if (int3D[index] < intensityLower * avgInt) {
			//ghost3D.push_back(pos3D[index]); 
			pos3D.Delete(index); int3D.erase(int3D.begin() + index); tempPredictions.Delete(index);
																						// shifting the corresponding activeLongTrack to inactiveLong or inactiveTracks
			if (activeLongTracks[index].Length() >= 7 && Distance(activeLongTracks[index].Last(), activeLongTracks[index].Penultimate()) < threshShift) {
				inactiveLongTracks.push_back(activeLongTracks[index]);
				a_il++;
			}
			else {
				inactiveTracks.push_back(activeLongTracks[index]);						// shifting the corresponding activeLongTrack to inactiveTracks
				a_is++;
			}
			activeLongTracks.erase(activeLongTracks.begin() + index);
			 s_al++;
			ghost++;
		}
	}

	deque<int> ignoreCam(pos3D.NumParticles());
	double intensityThresh = 0.25*_ipr.Get_threshold();
	// new version: Adding a check of intensity 
		// For a given predicted particles, if the intensity on one original image falls below a quarter of peak intensity threshold, ignore that camera for shaking
		// If it falls below on two cameras, move the track to inactive or inactiveLongTracks

	for (int i = 0; i < pos3D.NumParticles(); ) {										// deleting particles that are outside the image bounds on atleast 2 cameras
		double xlim = ((double)(Npixw - 1)) / 2, ylim = ((double)(Npixh - 1)) / 2;		// if the particle disappears only on 1 cam, ignore that cam and perform shaking
		int leftcams = 0, belowIntensity = 0; double shiftThreshold = pow(largestShift, 2);					
		ignoreCam[i] = ALL_CAMS;

		// out of bounds or below intensity threshold
		if (abs(pos3D[i].X1() - xlim) > xlim || abs(pos3D[i].Y1() - ylim) > ylim) {
			leftcams++; ignoreCam[i] = 0;
		}
		else if (pixels_orig[0][(int)round(pos3D[i].Y1())][(int)round(pos3D[i].X1())] < intensityThresh) {
			belowIntensity++; ignoreCam[i] = 0;
		}

		if (abs(pos3D[i].X2() - xlim) > xlim || abs(pos3D[i].Y2() - ylim) > ylim) {
			leftcams++; ignoreCam[i] = 1;
		}
		else if (pixels_orig[1][(int)round(pos3D[i].Y2())][(int)round(pos3D[i].X2())] < intensityThresh) {
			belowIntensity++; ignoreCam[i] = 1;
		}

		if (abs(pos3D[i].X3() - xlim) > xlim || abs(pos3D[i].Y3() - ylim) > ylim) {
			leftcams++; ignoreCam[i] = 2;
		}
		else if (pixels_orig[2][(int)round(pos3D[i].Y3())][(int)round(pos3D[i].X3())] < intensityThresh) {
			belowIntensity++; ignoreCam[i] = 2;
		}

		if (abs(pos3D[i].X4() - xlim) > xlim || abs(pos3D[i].Y4() - ylim) > ylim) {
			leftcams++; ignoreCam[i] = 3;
		}
		else if (pixels_orig[3][(int)round(pos3D[i].Y4())][(int)round(pos3D[i].X4())] < intensityThresh) {
			belowIntensity++; ignoreCam[i] = 3;
		}

		if (leftcams >= 2) {											// if the particle disappears on at least 2 cams (out of bounds only)
			pos3D.Delete(i); int3D.erase(int3D.begin() + i); tempPredictions.Delete(i);	// delete the particle and
																						// if the particles' translation b/w frames is within the threshold of largestParticleShift
			if (Distance(activeLongTracks[i].Last(), activeLongTracks[i].Penultimate()) <= shiftThreshold && Distance(activeLongTracks[i].Penultimate(), activeLongTracks[i].Antepenultimate()) <= shiftThreshold)
				exitTracks.push_back(activeLongTracks[i]);								// shift the corresponding activeLongTrack to exitTracks (or)
			else {
				inactiveTracks.push_back(activeLongTracks[i]);							// shift the corresponding activeLongTrack to inactiveTracks
				a_is++;
			}
			
			activeLongTracks.erase(activeLongTracks.begin() + i); s_al++;
			ignoreCam[i] = 100;
		}
		//else if (belowIntensity + leftcams >= 2) {					//  if the particle disappears on at least 2 cams (out of bounds or below intensity thresh)
		//	pos3D.Delete(i); int3D.erase(int3D.begin() + i); tempPredictions.Delete(i);	// delete the particle and
		//																				// if the track is longer than 12 frames and particles' translation b/w frames is within the threshold of largestParticleShift
		//	if (activeLongTracks[i].Length() >= 12 && Distance(activeLongTracks[i].Last(), activeLongTracks[i].Penultimate()) <= shiftThreshold) {
		//		inactiveLongTracks.push_back(activeLongTracks[i]);	s3++;				// shift the corresponding activeLongTrack to inactiveLongTracks (or)
		//	}
		//	else {
		//		inactiveTracks.push_back(activeLongTracks[i]);							// shift the corresponding activeLongTrack to inactiveTracks
		//		a3++;
		//	}

		//	activeLongTracks.erase(activeLongTracks.begin() + i); s2++;
		//	ignoreCam[i] = 100;
		//}
		else
			i++;
	}

	return ignoreCam;
}

// performing IPR on the residual images after removing tracked particles
Frame STB::IPRonResidual(Calibration& calib, Tiff2DFinder& t, deque<int**>& pixels_orig, deque<int**>& pixels_reproj, deque<int**>& pixels_res, Frame& estimates) {
	double mindist_3D = _ipr.mindist_3D;														// performing triangulation / IPR on the remaining particles in residual images
	double mindist_2D = _ipr.mindist_2D;
	double xlim = ((double)(Npixw - 1)) / 2, ylim = ((double)(Npixh - 1)) / 2;
	double intensityThresh = 0.25*_ipr.Get_threshold();
	deque<Position> candidates;
	deque<int> camNums;
	for (int i = 0; i < ncams; i++)
		camNums.push_back(i);

	for (int outerloop = 0; outerloop < _ipr.it_outerloop; outerloop++) {						// identify the particle candidates from residual images 

		calib.Set_min2D(pow(1.1, outerloop)*mindist_2D);

		// running IPR on the residual images
		Frame temp = _ipr.IPRLoop(calib, OTFcalib, camNums, ALL_CAMS, t.Get_colors(), pixels_orig, pixels_reproj, pixels_res);
		candidates.insert(candidates.end(), temp.begin(), temp.end());
	}

	if (_ipr.reducedCams) {																		// ipr with reduced cams

		Calibration calibReduced(_ipr.calibfile, REDUCED_CAMS, _ipr.mindist_2Dr, _ipr.mindist_3Dr, ncams);		// new calibration file for reduced cameras					
		for (int outerloop = 0; outerloop < _ipr.it_reducedCam; outerloop++) {

			calibReduced.Set_min2D(pow(1.1, outerloop)*_ipr.mindist_2Dr);

																								// running IPR by ignoring cameras one by one
			for (int ignoreCam = 0; ignoreCam < ncams; ignoreCam++) {
				Frame temp = _ipr.IPRLoop(calibReduced, OTFcalib, camNums, ignoreCam, t.Get_colors(), pixels_orig, pixels_reproj, pixels_res);																								
				
				//if (ignoreCam == 0)																// if the projection of a particle candidate on ignored cam is within its bounds and finds no particle there,
				//	for (int i = temp.NumParticles()-1; i >= 0 ; i--) 							// then delete the particle candidate						
				//		if (abs(temp[i].X1() - xlim) <= xlim && abs(temp[i].Y1() - ylim) <= ylim && pixels_orig[0][(int)round(temp[i].Y1())][(int)round(temp[i].X1())] < intensityThresh)
				//			temp.Delete(i);

				//if (ignoreCam == 1)																
				//	for (int i = temp.NumParticles() - 1; i >= 0; i--) 												
				//		if (abs(temp[i].X2() - xlim) <= xlim && abs(temp[i].Y2() - ylim) <= ylim && pixels_orig[1][(int)round(temp[i].Y2())][(int)round(temp[i].X2())] < intensityThresh)
				//			temp.Delete(i);

				//if (ignoreCam == 2)																
				//	for (int i = temp.NumParticles() - 1; i >= 0; i--) 												
				//		if (abs(temp[i].X3() - xlim) < xlim && abs(temp[i].Y3() - ylim) < ylim && pixels_orig[2][(int)round(temp[i].Y3())][(int)round(temp[i].X3())] < intensityThresh)
				//			temp.Delete(i);

				//if (ignoreCam == 3)															
				//	for (int i = temp.NumParticles() - 1; i >= 0; i--) 												
				//		if (abs(temp[i].X4() - xlim) < xlim && abs(temp[i].Y4() - ylim) < ylim && pixels_orig[3][(int)round(temp[i].Y4())][(int)round(temp[i].X4())] < intensityThresh)
				//			temp.Delete(i);
					
				candidates.insert(candidates.end(), temp.begin(), temp.end());
			}
		}

	}

	for (int i = 0; i < candidates.size(); i++) {												// removing a candidate if it's within 1 pixel of tracked particles
		double X1 = candidates[i].X1(), X2 = candidates[i].X2(), X3 = candidates[i].X3(), X4 = candidates[i].X4();
		double Y1 = candidates[i].Y1(), Y2 = candidates[i].Y2(), Y3 = candidates[i].Y3(), Y4 = candidates[i].Y4();
		for (int j = 0; j < estimates.NumParticles(); j++)
			if ((abs(X1 - estimates[j].X1()) < 1 && abs(Y1 - estimates[j].Y1()) < 1) ||
				(abs(X2 - estimates[j].X2()) < 1 && abs(Y2 - estimates[j].Y2()) < 1) ||
				(abs(X3 - estimates[j].X3()) < 1 && abs(Y3 - estimates[j].Y3()) < 1) ||
				(abs(X4 - estimates[j].X4()) < 1 && abs(Y4 - estimates[j].Y4()) < 1))
				candidates.erase(candidates.begin() + i);
	}

	return Frame(candidates);
}

// linking short tracks with particle candidates in residual images
void STB::MakeShortLinkResidual(int nextFrame, Frame& candidates, deque<Track>::iterator& tr, int iterations) {

	pair<Frame::const_iterator, float> cost;

	for (int it = 0; it < iterations; it++) {													// iteratively trying to find a link for the short track from particle candidates
		double rsqr = pow(pow(1.1, it) * 3 * avgIPDist, 2);
		double shift = pow(1.1, it) * largestShift;
		deque<double> dist;
		double totalDist = 0;
		deque<Position> disp;
		Position vel(0, 0, 0);																	// calculating the predictive vel. field as an avg. of particle velocities from neighbouring tracks
		double d;

		for (int j = 0; j < activeLongTracks.size(); j++) {										// identifying the neighbouring tracks (using 3*avg interparticle dist.) and getting their particle velocities
			double dsqr = Distance(tr->Last(), activeLongTracks[j].Penultimate());
			if (dsqr < rsqr && dsqr > 0) {
				d = pow(dsqr, 0.5);
				totalDist = totalDist + d;
				dist.push_back(d);
				disp.push_back(activeLongTracks[j].Last() - activeLongTracks[j].Penultimate());
			}
		}

		if (dist.size() > 0) {															// if it finds neighbouring tracks
			for (int j = 0; j < dist.size(); j++) {												// perform Gaussian wt. avg. to get velocity field
				double weight = (dist[j] / totalDist);
				vel.Set_X(vel.X() + weight*disp[j].X());
				vel.Set_Y(vel.Y() + weight*disp[j].Y());
				vel.Set_Z(vel.Z() + weight*disp[j].Z());
			}

			Position estimate = tr->Last() + vel;	
																								// finding a link for this short track using the velocity field
			cost = ComputeCost(candidates, candidates, searchRadiusSTB, estimate, vel, vel, true);			
		}

		else {																			// if no neighbouring tracks are identified
			Position estimate = tr->Last();														// using nearest neighbour to make a link
			cost = ComputeCost(candidates, candidates, shift, estimate, vel, vel, true);
		}

		if (cost.second != UNLINKED) {															// if linked with a candidate
			tr->AddNext(*cost.first, nextFrame);												// add the candidate to the track, delete it from list of untracked candidates and break the loop
			candidates.Delete(cost.first.where());
			++tr;
			break;
		}
	}

	if (cost.second == UNLINKED) {																// if no link is found for the short track
		if (tr->Length() > 3) {																	// and if the track has at least 4 particles
			inactiveTracks.push_back(*tr);														// add it to inactive tracks
			a_is++;
		}
		if (tr->Length() == 1)
			s_as1++;
		else if (tr->Length() == 2)
			a_as2++;
		else if (tr->Length() == 3)
			s_as3++;
		tr = activeShortTracks.erase(tr);														// then delete from activeShortTracks
		
	}
}

//########################### MAT / TXT FILES ###################################

void STB::MatTracksSave(string address, string s, int lastFrame) {
	// Saving tracks for Matlab
	string X1 = "ActiveLongTracks" + s, X2 = "ActiveShortTracks" + s, X3 = "InactiveTracks" + s, X4 = "exitTracks" + s, X5 = "InactiveLongTracks" + s;
	
	MatfileSave(activeLongTracks, address + X1, X1, lastFrame);
	MatfileSave(activeShortTracks, address + X2, X2, lastFrame);
	MatfileSave(inactiveTracks, address + X3, X3, lastFrame);
	MatfileSave(exitTracks, address + X4, X4, lastFrame);
	MatfileSave(inactiveLongTracks, address + X5, X5, lastFrame);
	
}


void STB::MatfileSave(deque<Track> tracks, string address, string name, int size) {
/*
 * Modified by Shiyong Tan, 2/8/18
 * Discard using matio, use Data_IO instead
 * Start:
 */
//	// Create a .mat file with pos3D
//	size_t sizeofpos3D = tracks.size();
//	double* tempX = new double[size];
//	double* tempY = new double[size];
//	double* tempZ = new double[size];
//	mat_t    *matfp;
//	matvar_t *cell_arrayX, *cell_elementX, *cell_arrayY, *cell_elementY, *cell_arrayZ, *cell_elementZ;
//	size_t dims[2] = { sizeofpos3D, 1 };
//	string mat_nameX = name + "X";
//	string mat_nameY = name + "Y";
//	string mat_nameZ = name + "Z";
//	matfp = Mat_CreateVer((address +".mat").c_str(), NULL, MAT_FT_DEFAULT);
//
//	switch (NULL == matfp) {
//		fprintf(stderr, "Error creating MAT file \"Tracking\"!\n");
//		break;
//	}
//	cell_arrayX = Mat_VarCreate(mat_nameX.c_str(), MAT_C_CELL, MAT_T_CELL, 2, dims, NULL, 0);
//	cell_arrayY = Mat_VarCreate(mat_nameY.c_str(), MAT_C_CELL, MAT_T_CELL, 2, dims, NULL, 0);
//	cell_arrayZ = Mat_VarCreate(mat_nameZ.c_str(), MAT_C_CELL, MAT_T_CELL, 2, dims, NULL, 0);
//	if (NULL == cell_arrayX || NULL == cell_arrayY || NULL == cell_arrayZ) {
//		fprintf(stderr, "Error creating variable for 'Tracking'\n");
//	}
//	else {
//		for (int i = 0; i < sizeofpos3D; i++) {
//			dims[0] = 1;
//			dims[1] = size;
//			int time = 0;
//			for (int k = 0; k < size; k++) {
//				if (time < tracks[i].Length() && k == tracks[i].GetTime(time) - 1) {
//					tempX[k] = tracks[i][time].X();
//					tempY[k] = tracks[i][time].Y();
//					tempZ[k] = tracks[i][time].Z();
//					time++;
//				}
//				else {
//					tempX[k] = 0;
//					tempY[k] = 0;
//					tempZ[k] = 0;
//				}
//			}
//
//			cell_elementX = Mat_VarCreate(NULL, MAT_C_DOUBLE, MAT_T_DOUBLE, 2, dims, tempX, 0);
//			cell_elementY = Mat_VarCreate(NULL, MAT_C_DOUBLE, MAT_T_DOUBLE, 2, dims, tempY, 0);
//			cell_elementZ = Mat_VarCreate(NULL, MAT_C_DOUBLE, MAT_T_DOUBLE, 2, dims, tempZ, 0);
//			switch (NULL == cell_elementX || NULL == cell_elementY || NULL == cell_elementZ) {
//				fprintf(stderr, "Error creating cell element variable\n");
//				Mat_VarFree(cell_arrayX); //Mat_VarFree(cell_arrayY); Mat_VarFree(cell_arrayZ);
//				Mat_Close(matfp);
//				break;
//			}
//			Mat_VarSetCell(cell_arrayX, i, cell_elementX);
//			Mat_VarSetCell(cell_arrayY, i, cell_elementY);
//			Mat_VarSetCell(cell_arrayZ, i, cell_elementZ);
//		}
//	}
//
//	Mat_VarWrite(matfp, cell_arrayX, MAT_COMPRESSION_NONE);
//	Mat_VarWrite(matfp, cell_arrayY, MAT_COMPRESSION_NONE);
//	Mat_VarWrite(matfp, cell_arrayZ, MAT_COMPRESSION_NONE);
//	Mat_VarFree(cell_arrayX); Mat_VarFree(cell_arrayY); Mat_VarFree(cell_arrayZ);
//	Mat_Close(matfp);
//	delete[] tempX, tempY, tempZ;
	// TODO: to check whether it works.

	// convert track into 3D matrix the time for each track is the same.
	size_t sizeofpos3D = tracks.size(); // the number of particle
	double track_data[sizeofpos3D][size][3];  // size is the total number of frames
	for(int i = 0; i < sizeofpos3D; i++) {
		int absolute_starttime = tracks[i].GetTime(0); // 0 is the starting time of the begining of the motion of a particle
							// absolute start time is the start time of a particle in the overall time reference for all particles.
		int absolute_endtime = tracks[i].GetTime(tracks[i].Length());
							//tracks[i].Length() is the end of the motion of a particle
							// absolute end time is the end time of a particle in the overall time reference for all particle
		for(int j = 0; j < size; j++) {
			if (absolute_starttime <= j + 1 && j + 1 <= absolute_endtime) {
				track_data[i][j][0] = tracks[i][j].X();
				track_data[i][j][1] = tracks[i][j].Y();
				track_data[i][j][2] = tracks[i][j].Z();
			} else { // the frame the particle doesn't show up is set as 0.
				track_data[i][j][0] = 0;
				track_data[i][j][1] = 0;
				track_data[i][j][2] = 0;
			}
		}
	}

	double dimension_info[3]; // a vector to save dimension info, this vector will tell how to read data
	dimension_info[0] = sizeofpos3D; // number of elements in 1st dimension
	dimension_info[1] = size; // number of elements in 2nd dimension
	dimension_info[2] = 3; // number of elements in 3rd dimension

	NumDataIO<double> data_io;
	data_io.SetFilePath(address + ".txt");

	// to save dimension info
	data_io.SetTotalNumber(4);
	data_io.WriteData((double*) dimension_info);

	// to append data
	data_io.SaveMode(1); //to set the save mode as append
	data_io.SetTotalNumber(sizeofpos3D * size * 3);
	data_io.WriteData((double*) track_data);

	// End
}


//###################### TEMPORARY FUNTIONS FOR TESTING ###############################

// to load 3D positions without IPR
Frame STB::Load_3Dpoints(string path) {
	/*
	 * Modified by Shiyong Tan, 2/8/18
	 * Discard using matio, use Data_IO instead
	 * Start:
	 */
	Frame iprFrame;
//	//string file = "S:/Projects/Bubble/10.31.17/Run1/BubblesNParticlesLow_4000fps/ParticlesOnly/" + path + ".mat";
//	string file = tiffaddress + path + ".mat";
//	const char *fileName = file.c_str();
//	mat_t *mat = Mat_Open(fileName, MAT_ACC_RDONLY);
//
//	if (mat == NULL) {
//		cout << " error in reading the mat file" << endl;
//	}
//	string cam;
//	string name;
//
//	if (mat) {
//		//std::cout << "Open file to read\n\tmat == " << mat << "\n";
//
//		matvar_t *matVar = 0;
//		name = path;
//		matVar = Mat_VarRead(mat, (char*)name.c_str());
//
//		if (matVar) {
//			int rows;	int cols;
//			rows = matVar->dims[0]; cols = matVar->dims[1];
//			unsigned namesize = matVar->nbytes / matVar->data_size;
//			double *namedata = static_cast<double*>(matVar->data);
//			for (int i = 0; i < rows; i++) {
//				Position pos(namedata[i], namedata[rows + i], namedata[2*rows + i]);
//				iprFrame.Add(pos);
//			}
//		}
//	}
//	else {
//		cout << "Cannot open file\n";
//	}
//
//	Mat_Close(mat);
	// TODO: to check whether it works.
	string file = tiffaddress + path + ".txt";
	NumDataIO<double> data_io;
	data_io.SetFilePath(file);
	int total_num = data_io.GetTotalNumber();
	int rows = total_num / 3;
	double points_array[rows][3];
	data_io.ReadData((double*) points_array);
	for (int i = 0; i < rows; i++) {
		Position pos(points_array[i][0], points_array[i][1], points_array[i][2]);
		iprFrame.Add(pos);
	}
	return iprFrame;
	// END
}

// Loading the tracks from .mat file (For Testing)
void STB::Load_Tracks(string path, TrackType trackType) {
	/*
	 * Modified by Shiyong Tan, 2/8/18
	 * Discard using matio, use Data_IO instead
	 * Start:
	 */
//	cout << "Loading the tracks from .mat files" << endl;
//	string file = tiffaddress + path + ".mat";
//	const char *fileName = file.c_str();
//	mat_t *mat = Mat_Open(fileName, MAT_ACC_RDONLY);
//
//	if (mat == NULL) {
//		cout << " error in reading the mat file" << endl;
//	}
//
//	string namex, namey, namez;
//
//	if (mat) {
//		//std::cout << "Open file to read\n\tmat == " << mat << "\n";
//
//		matvar_t *matVarx = 0, *matVary = 0, *matVarz = 0;
//		namex = path + "X"; namey = path + "Y"; namez = path + "Z";
//		matVarx = Mat_VarRead(mat, (char*)namex.c_str());
//		matVary = Mat_VarRead(mat, (char*)namey.c_str());
//		matVarz = Mat_VarRead(mat, (char*)namez.c_str());
//
//		if (matVarx && matVary && matVarz) {
//			int rowsx = matVarx->dims[0], colsx = matVarx->dims[1];
//			double *namedatax = static_cast<double*>(matVarx->data);
//
//			int rowsy = matVary->dims[0], colsy = matVary->dims[1];
//			double *namedatay = static_cast<double*>(matVary->data);
//
//			int rowsz = matVarz->dims[0], colsz = matVarz->dims[1];
//			double *namedataz = static_cast<double*>(matVarz->data);
//
//			for (int i = 0; i < rowsx; i++) {
//				int time = 1;
//				Track track;
//				for (int j = 0; j < colsx; j++) {
//					int element = i + j*rowsx;
//					if (namedatax[element] == 0) {
//						time++; continue;
//					}
//
//					Position pos(namedatax[element], namedatay[element], namedataz[element]);
//					track.AddNext(pos, time);
//					time++;
//				}
//
//				switch (trackType)
//				{
//					case ActiveLong : activeLongTracks.push_back(track);
//										break;
//					case ActiveShort: activeShortTracks.push_back(track);
//										break;
//					case Inactive: inactiveTracks.push_back(track);
//										break;
//					case Exit: exitTracks.push_back(track);
//										break;
//					case InactiveLong: inactiveLongTracks.push_back(track);
//										break;
//				}
//			}
//		}
//	}
//	else {
//		cout << "Cannot open file\n";
//	}
//
//	Mat_Close(mat);

	NumDataIO<double> data_io;
	data_io.SetFilePath(tiffaddress + path + ".txt");

	//read the dimension info
	double dimension_info[3];
	data_io.SetTotalNumber(3);
	data_io.ReadData((double*) dimension_info);

	int num_particles = (int) dimension_info[0];
	int num_frames = (int) dimension_info[1];

	double track_data[num_particles][num_frames][3];
	data_io.SetTotalNumber(num_particles * num_frames * 3);
	data_io.SetSkipDataNum(3); // skip the dimension info
	data_io.ReadData((double*) track_data);

	// convert 3D data into track
	for (int i = 0; i < num_particles; i++) {
		Track track;
		int time = 0;
		for (int j = 0; j < num_frames; j++) {
			if (track_data[i][j][0] == 0 &&
				track_data[i][j][1] == 0 &&
				track_data[i][j][2] == 0 ) { // if all of the data is 0, that means there is no track
				time++; continue;
			}
			Position pos(track_data[i][j][0], track_data[i][j][1], track_data[i][j][2]);
			track.AddNext(pos, time);
			time++;
		}
		switch (trackType)
		{
			case ActiveLong : activeLongTracks.push_back(track); break;
			case ActiveShort: activeShortTracks.push_back(track);break;
			case Inactive: inactiveTracks.push_back(track);break;
			case Exit: exitTracks.push_back(track);break;
			case InactiveLong: inactiveLongTracks.push_back(track);break;
		}
}
	//END
}