%% Function for creating an obstacle of the form of a square
function [ fu ] = obstaclesquare( x00,y00,r00 )

    fu = @fuu;

    function z = fuu(conf,x,y,z)
        x0 = conf.lx*x00;
        y0 = conf.ly*y00;
        r0 = conf.ly*r00;
        
        %% Create obstacles
        for i=1:length(x)
            for j=1:length(y)
                if(abs(x(i)-x0)<r0&&abs(y(j)-y0)<r0)
                    z(i,j) = 0;
                end
            end
        end
        
        
    end


end

