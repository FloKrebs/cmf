

// Copyright 2010 by Philipp Kraft
// This file is part of cmf.
//
//   cmf is free software: you can redistribute it and/or modify
//   it under the terms of the GNU General Public License as published by
//   the Free Software Foundation, either version 2 of the License, or
//   (at your option) any later version.
//
//   cmf is distributed in the hope that it will be useful,
//   but WITHOUT ANY WARRANTY; without even the implied warranty of
//   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//   GNU General Public License for more details.
//
//   You should have received a copy of the GNU General Public License
//   along with cmf.  If not, see <http://www.gnu.org/licenses/>.
//   
#include "ReachType.h"
#include <cmath>
#include "../math/real.h"
#include <stdexcept>
#define sqr(a) ((a)*(a))

double cmf::river::IChannel::qManning( double A,double slope ) const
{
	double 
		d=get_depth(A),
		P=get_wetted_perimeter(d)+0.001, // a mm extra to prevent divide by zero
		R=A/P+0.001;          // a mm extra to prevent pow failure
	return A*pow(R,2./3.)*sqrt(slope)/nManning;
}


/************************************************************************/
/* SWAT reach type                                                                     */
/************************************************************************/

/// \f[ d = \sqrt{\frac{A}{\Delta_{bank}} + \frac{{w_{bottom}}^2}{4 {\Delta_{bank}}^2}} - \frac{w_{bottom}}{2 \Delta_{bank}} \f]
/// If \f$d>d_{IChannel}\f$
/// \f[d=d_{IChannel}+\sqrt{\frac{A-A(d_{IChannel})}{\Delta_{flood\ plain}} + \frac{{w(d_{IChannel})}^2}{4 {\Delta_{flood\ plain}}^2}} - \frac{w(d_{IChannel})}{2 \Delta_{flood\ plain}} \f]
double cmf::river::SWATReachType::get_depth( double get_flux_crossection ) const
{
	//Calculate depth from the void volume of the IChannel, using the SWAT river geometry (trapezoid)
	double d = 
		sqrt(
			get_flux_crossection/BankSlope
			+ (BottomWidth*BottomWidth / (4 * BankSlope*BankSlope))
		)
		- BottomWidth/(2*BankSlope);
	//If the IChannel is full
	if (d>ChannelDepth)
	{
		//Calculate the get_flux_crossection of the full IChannel
		double A_ch=(BottomWidth+BankSlope*ChannelDepth)*ChannelDepth;
		//Calculate the width of the full IChannel
		double w_full=get_channel_width(ChannelDepth);
		//Calculate the sum of IChannel depth and depth of the floodplain filling
		d=ChannelDepth 
			+ sqrt((get_flux_crossection-A_ch)/FloodPlainSlope + w_full*w_full/(4*FloodPlainSlope*FloodPlainSlope))
			- w_full/(2*FloodPlainSlope);
	}
	return d;
}

/// If \f$d\le d_{Channel} \ f$
/// \f[P = w_{Bottom} + 2  \sqrt{1+ \Delta_{bank}^2} d \f]
/// else, if the river floods the flood plain
/// \f[P = P(d_{Channel} + 2 \sqrt{1+ {\Delta_{flood\ plain}}^2} (d-d_{Channel} \f]
double cmf::river::SWATReachType::get_wetted_perimeter( double depth ) const
{
	if (depth<=ChannelDepth) //All the water fits in the IChannel
		return BottomWidth + 2.*depth*sqrt(1+BankSlope*BankSlope);
	else	//The IChannel is full, and water flows on the flood plain
		return get_wetted_perimeter(ChannelDepth) + 2.*(depth-ChannelDepth)*sqrt(1+FloodPlainSlope*FloodPlainSlope);
}
/// If \f$d\le d_{Channel} \ f$
/// \f[w = w_{Bottom} + 2  \Delta_{bank} d \f]
/// else, if the river floods the flood plain
/// \f[w = w(d_{Channel}) + 2 \Delta_{flood\ plain} (d-d_{Channel}) \f]
double cmf::river::SWATReachType::get_channel_width( double depth ) const
{
	if (depth<=ChannelDepth) //All the water fits in the IChannel
		return BottomWidth + 2.*BankSlope*depth;
	else	//The IChannel is full, and water flows on the flood plain
		return get_channel_width(ChannelDepth) + 2.*FloodPlainSlope*(depth-ChannelDepth);
}
/// If \f$d\le d_{Channel} \ f$
/// \f[A = \left(w_{Bottom} + Delta_{bank} d\right) d \f]
/// else, if the river floods the flood plain
/// \f[P = P(d_{Channel}) + \left(w(d_{Channel} + Delta_{flood\ plain} \left(d-d_{Channel}\right)\right) (d-d_{Channel}) \f]
double cmf::river::SWATReachType::get_flux_crossection( double depth ) const
{
	if (depth<=ChannelDepth) //All the water fits in the IChannel
		return (BottomWidth + BankSlope*depth) * depth;
	else	//The IChannel is full, and water flows on the flood plain
		return get_flux_crossection(ChannelDepth) + (get_channel_width(ChannelDepth)+FloodPlainSlope*(depth-ChannelDepth))*(depth-ChannelDepth);
}

cmf::river::SWATReachType::SWATReachType(double l) :IChannel(l), BottomWidth(3.0),ChannelDepth(0.5),BankSlope(2.0),FloodPlainSlope(200.0)
{

}

cmf::river::SWATReachType::SWATReachType(double l, double BankWidth,double get_depth ) : IChannel(l), BottomWidth(3.0),ChannelDepth(0.5),BankSlope(2.0),FloodPlainSlope(100.0)
{
	ChannelDepth=get_depth;
	BottomWidth=BankWidth-2*BankSlope*get_depth;
	///For very narrow reaches (w/d is small) use steeper banks, minimum bottom width=0.5 bank width
	if (BottomWidth<0.5*BankWidth)
	{
		BottomWidth=0.5*BankWidth;
		BankSlope=(BankWidth-BottomWidth)/(2*get_depth);
	}
}

/************************************************************************/
/* Triangular shape                                                                     */
/************************************************************************/


/// \f{eqnarray*}
/// w &=& 2 \Delta\ d
/// \f}
double cmf::river::TriangularReach::get_channel_width( double depth ) const
{
	return 2*BankSlope * depth;
}
/// \f{eqnarray*}
/// P &=& 2 d \sqrt{1+\Delta^2}
/// \f}
double cmf::river::TriangularReach::get_wetted_perimeter( double depth ) const
{
	return 2*depth*sqrt(1+BankSlope*BankSlope);
}
/// \f{eqnarray*}
/// d &=& \sqrt{\frac{A}{\Delta}}
/// \f}
double cmf::river::TriangularReach::get_depth( double area ) const
{
	return sqrt(area/BankSlope);
}
/// \f{eqnarray*}
/// A &=& d^2 \Delta
/// \f}
double cmf::river::TriangularReach::get_flux_crossection( double depth ) const
{
	return depth * depth * BankSlope;
}

cmf::river::TriangularReach::TriangularReach(double l, double bankSlope/*=2*/ ) : IChannel(l),BankSlope(bankSlope)
{

}

/************************************************************************/
/* Rectangular Reach                                                                     */
/************************************************************************/

double cmf::river::RectangularReach::get_channel_width( double depth ) const
{
	return m_width;
}

double cmf::river::RectangularReach::get_wetted_perimeter( double depth ) const
{
	return 2*depth+m_width;
}

double cmf::river::RectangularReach::get_depth( double area ) const
{
	return area/m_width;
}

double cmf::river::RectangularReach::get_flux_crossection( double depth ) const
{
	return depth*m_width;
}

cmf::river::RectangularReach* cmf::river::RectangularReach::copy() const
{
	return new RectangularReach(length, m_width);
}
/************************************************************************/
/* Pipe                                                                 */
/************************************************************************/
double cmf::river::PipeReach::get_channel_width( double depth ) const
{
	if (depth<0 || depth>radius*2)
		return 0;
	else
		return 2*sqrt(fabs(sqr(radius)-sqr(radius-depth)));
}

double cmf::river::PipeReach::get_wetted_perimeter( double depth ) const
{
	if (depth<=0)
		return 0;
	else if (depth>=2*radius)
		return 2*Pi*radius;
	else
		return acos((radius-depth)/radius)*radius;
}

double cmf::river::PipeReach::get_depth( double area ) const
{
	if (area>=Pi*sqr(radius)) 
		return 2*radius;
	else if (area<=0)
		return 0;
	else
		return radius*(1-cos(area/sqr(radius)));
}

double cmf::river::PipeReach::get_flux_crossection( double depth ) const
{
	if (depth<=0)
		return 0;
	else if (depth>=2*radius)
		return Pi*sqr(radius);
	else
		return acos((radius-depth)/radius)*sqr(radius);
}
cmf::river::PipeReach* cmf::river::PipeReach::copy() const 
{ 
	return new PipeReach(length,2*radius);
}

/************************************************************************/
/* Channel                                                              */
/************************************************************************/

cmf::river::Channel::Channel( char typecode, double length, double width/*=1.*/,double depth/*=0.25*/ )
: IChannel(length)
{
	IChannel* newChannel=0;
	switch(typecode)
	{
	case 'T': 
		newChannel = new cmf::river::TriangularReach(length); 
		break;
	case 'R':	
		newChannel = new cmf::river::RectangularReach(length,width);	 
		break;
	case 'S':	
		newChannel= new cmf::river::SWATReachType(length,width,depth);
		break;
	case 'P': 
		newChannel = new cmf::river::PipeReach(length,width);
		break;
	default:
		throw std::runtime_error("Not supported reach type shortcut");
		break;
	};
	m_channel.reset(newChannel);

}

cmf::river::Channel::Channel( const Channel& for_copy ) 
: IChannel(for_copy.length), m_channel(for_copy.m_channel->copy())
{

}

cmf::river::Channel::Channel( const IChannel& for_wrapping ) 
: IChannel(for_wrapping.length), m_channel(for_wrapping.copy())
{

}

cmf::river::Channel::Channel( const IVolumeHeightFunction& for_casting ) : IChannel(0.0)
{
	const IChannel *cast = dynamic_cast<const IChannel *>(&for_casting);
	if(cast == 0)
		throw std::runtime_error("Failed interpreting a non channel h(V) function as a channel");
	else
	{
		m_channel.reset(cast->copy());
		length = cast->length;
	}
}
cmf::river::Channel& cmf::river::Channel::operator=( const IChannel& for_assignment )
{
	m_channel.reset(for_assignment.copy());
	return *this;
}

cmf::river::MeanChannel::MeanChannel( const IChannel& channel1,const IChannel& channel2 ) 
: IChannel(mean(channel1.length,channel2.length)), m_channel1(channel1),m_channel2(channel2)
{}

cmf::river::MeanChannel::MeanChannel( const MeanChannel& meanChannel ) 
: IChannel(meanChannel.length), m_channel1(meanChannel.m_channel1), m_channel2(meanChannel.m_channel2)
{		 }

char cmf::river::MeanChannel::typecode() const {	return 'M';}

double cmf::river::MeanChannel::get_channel_width( double depth ) const {
	return mean(m_channel1.get_channel_width(depth),m_channel2.get_channel_width(depth));
}

double cmf::river::MeanChannel::get_depth( double area ) const					{
	return mean(m_channel1.get_depth(area),m_channel2.get_depth(area));
}

double cmf::river::MeanChannel::get_flux_crossection( double depth ) const {
	return mean(m_channel1.get_flux_crossection(depth),m_channel2.get_flux_crossection(depth));
}

double cmf::river::MeanChannel::get_wetted_perimeter( double depth ) const {
	return mean(m_channel1.get_wetted_perimeter(depth),m_channel2.get_wetted_perimeter(depth));
}

cmf::river::MeanChannel* cmf::river::MeanChannel::copy() const
{
	return new MeanChannel(*this);
}