in general there are some objects like: river, cloud, lodge, etc.
moving objects: cloud, smoke, river, trees (i mean shivering when the wind blows)
object depends on several parameters
basic version is only wind_blows which is randomly generated each maybe 5 seconds for a random
interval of [3 to 5] (maybe)


each object is (matrix with symbols, layer (as a float number))
each object is associated with a function render_something
which returns an 



the main program opens a full-screen window
then it retrieves the sizes of the window (rows and columns) and renders a message.
the general object is a resulting matrix [rows, cols] which is rendered when the changes appear

there should be env variables: alive (continue looping) and wind_blows

before rendering all the layers are sorted from max to min
so when rendering we compare we go for each i, j in rows, cols and look if the letter
in the first layer exists and is not a space. Space means emptiness.

each object is associated with a thread and a function render_something
when the changes appear in the thread, it locks the main mutex and gives a new submatrix to a
function render() with parameters: where that submatrix begins
